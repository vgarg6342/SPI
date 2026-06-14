/*
 * pru_spi_fw.c — PRU0 Firmware: Bit-Banged SPI Master with 4 CS Lines
 *
 * This firmware runs on PRU0 of the BeagleBone Black AM335x.
 * It implements a full-duplex SPI master via bit-banging using the
 * PRU's direct GPIO registers (R30 output, R31 input).
 *
 * Data flow:
 *   1. ARM writes TX data to DDR, fills command block in shared RAM
 *   2. PRU detects command, copies DDR data → local SRAM in chunks
 *   3. PRU bit-bangs SPI from local SRAM (deterministic timing)
 *   4. PRU copies RX data from local SRAM → DDR
 *   5. PRU sets status = DONE
 *
 * Supports SPI modes 0-3, configurable clock speed, 4 CS lines.
 *
 * Compiled with: clpru (TI PRU C/C++ Compiler)
 *
 * Copyright (c) 2026 — MIT License
 */

#include <stdint.h>
#include <string.h>
#include <pru_cfg.h>
#include <pru_intc.h>
#include "resource_table.h"
#include "pru_spi_common.h"

/* -----------------------------------------------------------------------
 * PRU Direct I/O Registers
 *
 * __R30: Output register — controls PRU0 output pins
 * __R31: Input register  — reads PRU0 input pins
 *
 * These are special compiler-recognized register variables that map
 * directly to the hardware I/O registers. Single-cycle access.
 * ----------------------------------------------------------------------- */
volatile register uint32_t __R30;
volatile register uint32_t __R31;

/* -----------------------------------------------------------------------
 * Chip Select Bit Lookup Table
 * ----------------------------------------------------------------------- */
static const uint32_t cs_bits[NUM_CS_LINES] = CS_BIT_LOOKUP;

/* -----------------------------------------------------------------------
 * Pointers to shared and local memory
 *
 * PRU0 data RAM starts at 0x00000000 (from PRU's local perspective).
 * PRU shared RAM starts at 0x00010000.
 *
 * The command block is placed at the start of shared RAM.
 * TX/RX staging buffers are in PRU0 local data RAM.
 * ----------------------------------------------------------------------- */

/* Command block in shared RAM */
#define CMD_BLOCK_ADDR  0x00010000
volatile struct pru_spi_cmd *cmd =
    (volatile struct pru_spi_cmd *)CMD_BLOCK_ADDR;

/* Local SRAM staging buffers */
static uint8_t *tx_staging = (uint8_t *)SRAM_TX_OFFSET;
static uint8_t *rx_staging = (uint8_t *)SRAM_RX_OFFSET;

/* -----------------------------------------------------------------------
 * Memory Copy: DDR ↔ Local SRAM
 *
 * These functions use LBBO/SBBO instructions to transfer data between
 * DDR (via OCP port) and PRU local data RAM.
 *
 * NOTE: DDR access is ~43+ cycles per word and non-deterministic.
 *       This is done OUTSIDE the SPI bit-bang loop to maintain
 *       deterministic SPI timing.
 * ----------------------------------------------------------------------- */

/**
 * Copy 'len' bytes from a physical DDR address to local SRAM buffer.
 * PRU accesses DDR via the OCP master port — the physical address
 * is used directly.
 */
static void ddr_to_local(uint8_t *local_dst, uint32_t ddr_src_addr, uint32_t len)
{
    volatile uint8_t *src = (volatile uint8_t *)ddr_src_addr;
    uint32_t i;

    /* Copy word-aligned chunks for efficiency when possible */
    uint32_t words = len >> 2;
    uint32_t remainder = len & 3;
    volatile uint32_t *src32 = (volatile uint32_t *)src;
    uint32_t *dst32 = (uint32_t *)local_dst;

    for (i = 0; i < words; i++) {
        dst32[i] = src32[i];
    }

    /* Copy remaining bytes */
    uint32_t byte_offset = words << 2;
    for (i = 0; i < remainder; i++) {
        local_dst[byte_offset + i] = src[byte_offset + i];
    }
}

/**
 * Copy 'len' bytes from local SRAM buffer to a physical DDR address.
 */
static void local_to_ddr(uint32_t ddr_dst_addr, uint8_t *local_src, uint32_t len)
{
    volatile uint8_t *dst = (volatile uint8_t *)ddr_dst_addr;
    uint32_t i;

    uint32_t words = len >> 2;
    uint32_t remainder = len & 3;
    volatile uint32_t *dst32 = (volatile uint32_t *)dst;
    uint32_t *src32 = (uint32_t *)local_src;

    for (i = 0; i < words; i++) {
        dst32[i] = src32[i];
    }

    uint32_t byte_offset = words << 2;
    for (i = 0; i < remainder; i++) {
        dst[byte_offset + i] = local_src[byte_offset + i];
    }
}

/* -----------------------------------------------------------------------
 * SPI Bit-Bang Functions — One per SPI Mode
 *
 * Each function transfers a single byte (8 bits, MSB first) and returns
 * the received byte. They differ in CPOL (idle clock polarity) and
 * CPHA (sampling edge).
 *
 * The 'delay' parameter controls the clock half-period in PRU cycles.
 * ----------------------------------------------------------------------- */

/**
 * SPI Mode 0: CPOL=0 (idle LOW), CPHA=0 (sample on RISING edge)
 *
 * Timing:
 *   1. Set MOSI
 *   2. Delay (setup time)
 *   3. SCLK → HIGH (rising edge) — slave samples MOSI, master samples MISO
 *   4. Delay (hold time)
 *   5. SCLK → LOW (falling edge) — slave shifts next bit
 *   6. Repeat for 8 bits
 */
static inline uint8_t spi_xfer_byte_mode0(uint8_t tx_byte)
{
    uint8_t rx_byte = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        /* Set MOSI based on current TX bit */
        if (tx_byte & (1 << i))
            __R30 |= MOSI_BIT;
        else
            __R30 &= ~MOSI_BIT;

        /* Setup time delay */
        __delay_cycles(SCLK_DELAY_CYCLES);

        /* Rising edge: SCLK HIGH */
        __R30 |= SCLK_BIT;

        /* Sample MISO on rising edge */
        if (__R31 & MISO_BIT)
            rx_byte |= (1 << i);

        /* Hold time delay */
        __delay_cycles(SCLK_DELAY_CYCLES);

        /* Falling edge: SCLK LOW */
        __R30 &= ~SCLK_BIT;
    }

    return rx_byte;
}

/**
 * SPI Mode 1: CPOL=0 (idle LOW), CPHA=1 (sample on FALLING edge)
 *
 * Timing:
 *   1. SCLK → HIGH (rising edge) — slave latches MOSI
 *   2. Set MOSI
 *   3. Delay
 *   4. SCLK → LOW (falling edge) — master samples MISO
 *   5. Delay
 *   6. Repeat
 */
static inline uint8_t spi_xfer_byte_mode1(uint8_t tx_byte)
{
    uint8_t rx_byte = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        /* Rising edge: SCLK HIGH */
        __R30 |= SCLK_BIT;

        /* Set MOSI */
        if (tx_byte & (1 << i))
            __R30 |= MOSI_BIT;
        else
            __R30 &= ~MOSI_BIT;

        /* Delay */
        __delay_cycles(SCLK_DELAY_CYCLES);

        /* Falling edge: SCLK LOW */
        __R30 &= ~SCLK_BIT;

        /* Sample MISO on falling edge */
        if (__R31 & MISO_BIT)
            rx_byte |= (1 << i);

        /* Delay */
        __delay_cycles(SCLK_DELAY_CYCLES);
    }

    return rx_byte;
}

/**
 * SPI Mode 2: CPOL=1 (idle HIGH), CPHA=0 (sample on FALLING edge)
 *
 * Like Mode 0 but with inverted clock polarity.
 */
static inline uint8_t spi_xfer_byte_mode2(uint8_t tx_byte)
{
    uint8_t rx_byte = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        /* Set MOSI */
        if (tx_byte & (1 << i))
            __R30 |= MOSI_BIT;
        else
            __R30 &= ~MOSI_BIT;

        /* Setup delay */
        __delay_cycles(SCLK_DELAY_CYCLES);

        /* Falling edge: SCLK LOW */
        __R30 &= ~SCLK_BIT;

        /* Sample MISO on falling edge */
        if (__R31 & MISO_BIT)
            rx_byte |= (1 << i);

        /* Hold delay */
        __delay_cycles(SCLK_DELAY_CYCLES);

        /* Rising edge: SCLK HIGH (return to idle) */
        __R30 |= SCLK_BIT;
    }

    return rx_byte;
}

/**
 * SPI Mode 3: CPOL=1 (idle HIGH), CPHA=1 (sample on RISING edge)
 *
 * Like Mode 1 but with inverted clock polarity.
 */
static inline uint8_t spi_xfer_byte_mode3(uint8_t tx_byte)
{
    uint8_t rx_byte = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        /* Falling edge: SCLK LOW */
        __R30 &= ~SCLK_BIT;

        /* Set MOSI */
        if (tx_byte & (1 << i))
            __R30 |= MOSI_BIT;
        else
            __R30 &= ~MOSI_BIT;

        /* Delay */
        __delay_cycles(SCLK_DELAY_CYCLES);

        /* Rising edge: SCLK HIGH */
        __R30 |= SCLK_BIT;

        /* Sample MISO on rising edge */
        if (__R31 & MISO_BIT)
            rx_byte |= (1 << i);

        /* Delay */
        __delay_cycles(SCLK_DELAY_CYCLES);
    }

    return rx_byte;
}

/* -----------------------------------------------------------------------
 * Transfer a chunk of bytes using the selected SPI mode
 * ----------------------------------------------------------------------- */
static void spi_xfer_chunk(uint8_t *tx_buf, uint8_t *rx_buf,
                           uint32_t len, uint32_t mode)
{
    uint32_t i;

    switch (mode) {
    case SPI_MODE_0:
        for (i = 0; i < len; i++)
            rx_buf[i] = spi_xfer_byte_mode0(tx_buf[i]);
        break;
    case SPI_MODE_1:
        for (i = 0; i < len; i++)
            rx_buf[i] = spi_xfer_byte_mode1(tx_buf[i]);
        break;
    case SPI_MODE_2:
        for (i = 0; i < len; i++)
            rx_buf[i] = spi_xfer_byte_mode2(tx_buf[i]);
        break;
    case SPI_MODE_3:
        for (i = 0; i < len; i++)
            rx_buf[i] = spi_xfer_byte_mode3(tx_buf[i]);
        break;
    default:
        /* Should not reach here — validated before calling */
        for (i = 0; i < len; i++)
            rx_buf[i] = spi_xfer_byte_mode0(tx_buf[i]);
        break;
    }
}

/* -----------------------------------------------------------------------
 * Parallel (4-lane) Transfer — drive 4 DACs simultaneously
 *
 * The ARM side has already bit-transposed the four DAC data streams into a
 * stream of "R30-ready" words: each word holds the 4 MOSI lane bits for one
 * bit-time, positioned at PAR_MOSI0/1/2/3, with the SCLK and CS bits left at 0.
 * We blast them out against the shared clock — no per-bit math in the hot loop,
 * so all 4 lanes update with a single store and share each clock edge.
 *
 * The shared CS/SYNC (PAR_CS_BIT, active LOW) is driven by the PRU: it goes
 * LOW for the duration of each frame and HIGH between frames, so all 4 DACs
 * latch the same frame on the same edge. Because the stream words carry CS=0,
 * CS stays asserted automatically during the bit loop.
 *
 * Write-only: no MISO. 'frames' frames of 'fb' words each are sent.
 * ----------------------------------------------------------------------- */
static void spi_xfer_parallel_frames(uint32_t *stream, uint32_t frames,
                                      uint32_t fb, uint32_t mode)
{
    uint32_t f, k, base, w;

    w = 0;

    if (mode >= SPI_MODE_2) {
        /* CPOL=1: clock idles HIGH */
        for (f = 0; f < frames; f++) {
            base = f * fb;
            /* Assert CS (low); present first bit with clock idle HIGH */
            __R30 = stream[base] | PAR_SCLK_BIT;
            __delay_cycles(PAR_CS_SETUP_CYCLES);
            for (k = 0; k < fb; k++) {
                w = stream[base + k];
                __R30 = w | PAR_SCLK_BIT;       /* data, SCLK high, CS low */
                __delay_cycles(SCLK_DELAY_CYCLES);
                __R30 = w;                       /* falling edge (sample) */
                __delay_cycles(SCLK_DELAY_CYCLES);
            }
            __R30 = w | PAR_SCLK_BIT;            /* clock back to idle HIGH */
            __delay_cycles(PAR_CS_HOLD_CYCLES);
            __R30 = PAR_CS_BIT | PAR_SCLK_BIT;   /* CS HIGH (latch), lanes low */
            __delay_cycles(PAR_CS_GAP_CYCLES);
        }
    } else {
        /* CPOL=0: clock idles LOW */
        for (f = 0; f < frames; f++) {
            base = f * fb;
            /* Assert CS (low); present first bit with clock idle LOW */
            __R30 = stream[base];
            __delay_cycles(PAR_CS_SETUP_CYCLES);
            for (k = 0; k < fb; k++) {
                w = stream[base + k];
                __R30 = w;                       /* data, SCLK low, CS low */
                __delay_cycles(SCLK_DELAY_CYCLES);
                __R30 = w | PAR_SCLK_BIT;        /* rising edge (sample) */
                __delay_cycles(SCLK_DELAY_CYCLES);
            }
            __R30 = w;                           /* clock back to idle LOW */
            __delay_cycles(PAR_CS_HOLD_CYCLES);
            __R30 = PAR_CS_BIT;                  /* CS HIGH (latch), lanes low */
            __delay_cycles(PAR_CS_GAP_CYCLES);
        }
    }
}

/* -----------------------------------------------------------------------
 * Assert / Deassert a Chip Select Line
 *
 * CS lines are active LOW: assert = drive LOW, deassert = drive HIGH.
 * ----------------------------------------------------------------------- */
static inline void cs_assert(uint32_t cs_line)
{
    __R30 &= ~cs_bits[cs_line];
}

static inline void cs_deassert(uint32_t cs_line)
{
    __R30 |= cs_bits[cs_line];
}

static inline void cs_deassert_all(void)
{
    __R30 |= CS_ALL_BITS;
}

/* -----------------------------------------------------------------------
 * Initialize SPI Pins
 *
 * Set initial pin states:
 *   - All CS lines HIGH (deasserted, active low)
 *   - SCLK set according to idle polarity (will be adjusted per mode)
 *   - MOSI LOW
 * ----------------------------------------------------------------------- */
static void spi_init_pins(void)
{
    /* Deassert all chip selects (drive HIGH) */
    __R30 |= CS_ALL_BITS;

    /* SCLK LOW (Mode 0/1 idle state) */
    __R30 &= ~SCLK_BIT;

    /* MOSI LOW — single-MOSI lane and the 4 parallel-mode lanes */
    __R30 &= ~(MOSI_BIT | PAR_MOSI_ALL);

    /* Parallel-mode shared CS deasserted (HIGH, active low) */
    __R30 |= PAR_CS_BIT;
}

/**
 * Set SCLK idle state based on SPI mode (CPOL).
 * Modes 0,1: CPOL=0 → idle LOW
 * Modes 2,3: CPOL=1 → idle HIGH
 */
static inline void spi_set_idle_clock(uint32_t mode)
{
    if (mode >= SPI_MODE_2) {
        __R30 |= SCLK_BIT;   /* CPOL=1: idle HIGH */
    } else {
        __R30 &= ~SCLK_BIT;  /* CPOL=0: idle LOW */
    }
}

/* -----------------------------------------------------------------------
 * Validate Command Block
 * ----------------------------------------------------------------------- */
static uint32_t validate_command(void)
{
    if (cmd->magic != PRU_SPI_MAGIC)
        return ERR_BAD_MAGIC;
    if (cmd->cs_line >= NUM_CS_LINES)
        return ERR_INVALID_CS;
    if (cmd->spi_mode > SPI_MODE_3)
        return ERR_INVALID_MODE;
    if (cmd->transfer_len == 0)
        return ERR_ZERO_LENGTH;

    return ERR_NONE;
}

/* -----------------------------------------------------------------------
 * Execute a Full SPI Transfer
 *
 * Handles the complete transfer lifecycle:
 *   1. Validate parameters
 *   2. Set clock idle state for the requested SPI mode
 *   3. Assert CS
 *   4. For each SRAM_BUF_SIZE chunk:
 *      a. Copy TX data from DDR → local SRAM
 *      b. Bit-bang the chunk
 *      c. Copy RX data from local SRAM → DDR
 *   5. Deassert CS
 *   6. Report status
 * ----------------------------------------------------------------------- */
static void execute_transfer(void)
{
    uint32_t err;
    uint32_t total_len;
    uint32_t bytes_remaining;
    uint32_t chunk_size;
    uint32_t offset;
    uint32_t spi_mode;
    uint32_t cs_line;
    uint32_t tx_addr;
    uint32_t rx_addr;

    /* Mark as busy */
    cmd->status = STATUS_BUSY;
    cmd->bytes_done = 0;
    cmd->error_code = ERR_NONE;

    /* Validate */
    err = validate_command();
    if (err != ERR_NONE) {
        cmd->error_code = err;
        cmd->status = STATUS_ERROR;
        return;
    }

    /* Cache command parameters locally (they're volatile) */
    total_len = cmd->transfer_len;
    spi_mode  = cmd->spi_mode;
    cs_line   = cmd->cs_line;
    tx_addr   = cmd->tx_ddr_addr;
    rx_addr   = cmd->rx_ddr_addr;

    /* Set SCLK idle state for this mode */
    spi_set_idle_clock(spi_mode);

    /* Small delay before CS assertion */
    __delay_cycles(10);

    /* Assert chip select */
    cs_assert(cs_line);

    /* CS setup time */
    __delay_cycles(20);

    /* Transfer data in chunks to maintain deterministic SPI timing */
    bytes_remaining = total_len;
    offset = 0;

    while (bytes_remaining > 0) {
        /* Determine chunk size */
        chunk_size = bytes_remaining;
        if (chunk_size > SRAM_BUF_SIZE)
            chunk_size = SRAM_BUF_SIZE;

        /* Stage 1: Copy TX data from DDR → local SRAM */
        if (tx_addr != 0) {
            ddr_to_local(tx_staging, tx_addr + offset, chunk_size);
        } else {
            /* No TX data — fill with 0x00 (read-only transfer) */
            uint32_t k;
            for (k = 0; k < chunk_size; k++)
                tx_staging[k] = 0x00;
        }

        /* Stage 2: Bit-bang the chunk from local SRAM */
        spi_xfer_chunk(tx_staging, rx_staging, chunk_size, spi_mode);

        /* Stage 3: Copy RX data from local SRAM → DDR */
        if (rx_addr != 0) {
            local_to_ddr(rx_addr + offset, rx_staging, chunk_size);
        }

        /* Update progress */
        offset += chunk_size;
        bytes_remaining -= chunk_size;
        cmd->bytes_done = offset;
    }

    /* CS hold time */
    __delay_cycles(20);

    /* Deassert chip select */
    cs_deassert(cs_line);

    /* Return SCLK to idle */
    spi_set_idle_clock(spi_mode);

    /* Mark transfer complete */
    cmd->status = STATUS_DONE;
}

/* -----------------------------------------------------------------------
 * Execute a Parallel (4-lane) Write Transfer
 *
 * Streams a pre-transposed R30 word stream out to 4 DACs at once on the
 * shared clock, with the PRU driving the shared CS/SYNC (active LOW) — each
 * frame is framed by CS so all 4 DACs latch together. Write-only. The stream
 * lives at cmd->tx_ddr_addr and is num_frames * frame_bits words long.
 * ----------------------------------------------------------------------- */
static void execute_parallel_transfer(void)
{
    uint32_t spi_mode;
    uint32_t num_frames;
    uint32_t frame_bits;
    uint32_t tx_addr;
    uint32_t frames_per_chunk;
    uint32_t frames_remaining;
    uint32_t frame_offset;
    uint32_t *stream = (uint32_t *)tx_staging;

    /* Mark as busy */
    cmd->status = STATUS_BUSY;
    cmd->bytes_done = 0;
    cmd->error_code = ERR_NONE;

    /* Validate */
    if (cmd->magic != PRU_SPI_MAGIC) {
        cmd->error_code = ERR_BAD_MAGIC;
        cmd->status = STATUS_ERROR;
        return;
    }
    if (cmd->spi_mode > SPI_MODE_3) {
        cmd->error_code = ERR_INVALID_MODE;
        cmd->status = STATUS_ERROR;
        return;
    }
    if (cmd->tx_ddr_addr == 0) {
        cmd->error_code = ERR_ADDR_NULL;
        cmd->status = STATUS_ERROR;
        return;
    }

    spi_mode   = cmd->spi_mode;
    num_frames = cmd->num_frames;
    frame_bits = cmd->frame_bits;
    tx_addr    = cmd->tx_ddr_addr;

    if (num_frames == 0 || frame_bits == 0 || frame_bits > 32) {
        cmd->error_code = ERR_INVALID_FRAMES;
        cmd->status = STATUS_ERROR;
        return;
    }

    /* How many whole frames fit in the local SRAM staging buffer.
     * SRAM_BUF_SIZE bytes => SRAM_BUF_SIZE/4 words. We chunk on FRAME
     * boundaries so CS framing never splits across a DDR copy. */
    frames_per_chunk = (SRAM_BUF_SIZE / 4) / frame_bits;
    if (frames_per_chunk == 0)
        frames_per_chunk = 1;   /* frame_bits<=32 guarantees this holds */

    /* Idle: MOSI lanes low, CS HIGH (deasserted), clock at idle polarity */
    __R30 &= ~PAR_MOSI_ALL;
    __R30 |= PAR_CS_BIT;
    spi_set_idle_clock(spi_mode);
    __delay_cycles(10);

    frames_remaining = num_frames;
    frame_offset = 0;

    while (frames_remaining > 0) {
        uint32_t frames_now = frames_remaining;
        uint32_t words_now;

        if (frames_now > frames_per_chunk)
            frames_now = frames_per_chunk;
        words_now = frames_now * frame_bits;

        /* Copy this chunk of the stream from DDR -> local SRAM */
        ddr_to_local(tx_staging,
                     tx_addr + ((frame_offset * frame_bits) << 2),
                     words_now << 2);

        /* Shift these frames out on all 4 lanes, CS-framed by the PRU */
        spi_xfer_parallel_frames(stream, frames_now, frame_bits, spi_mode);

        frame_offset += frames_now;
        frames_remaining -= frames_now;
        cmd->bytes_done = (frame_offset * frame_bits) << 2;
    }

    /* Leave CS deasserted (HIGH) and clock at idle */
    __R30 |= PAR_CS_BIT;
    cmd->status = STATUS_DONE;
}

/* -----------------------------------------------------------------------
 * Main Entry Point
 * ----------------------------------------------------------------------- */
void main(void)
{
    /*
     * Enable OCP master port.
     *
     * The PRU's OCP master port is disabled by default. We must enable it
     * to allow the PRU to access DDR memory and other system resources
     * outside the PRU subsystem.
     *
     * CT_CFG is the PRU configuration register space.
     * SYSCFG.STANDBY_INIT = 0 enables the OCP master port.
     */
    CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

    /* Initialize SPI pins */
    spi_init_pins();

    /*
     * Initialize the command block.
     * Set magic and status so the ARM side knows the PRU is ready.
     */
    cmd->magic = PRU_SPI_MAGIC;
    cmd->command = CMD_IDLE;
    cmd->status = STATUS_IDLE;
    cmd->bytes_done = 0;
    cmd->error_code = ERR_NONE;
    cmd->spi_mode = SPI_MODE_0;

    /*
     * Main loop: poll for commands from the ARM host.
     *
     * The PRU runs at 200MHz and the polling loop is very fast (~3 cycles
     * per iteration), so latency from command issue to execution start
     * is ~15ns.
     */
    while (1) {
        /* Wait for a command */
        if (cmd->command == CMD_IDLE) {
            /* Spin — minimal power impact on PRU */
            continue;
        }

        if (cmd->command == CMD_SHUTDOWN) {
            /* Graceful shutdown */
            cs_deassert_all();
            __R30 &= ~(SCLK_BIT | MOSI_BIT);
            cmd->status = STATUS_DONE;
            cmd->command = CMD_IDLE;
            /* Halt the PRU */
            __halt();
            return;
        }

        if (cmd->command == CMD_TRANSFER) {
            /* Execute the single-MOSI SPI transfer */
            execute_transfer();

            /* Reset command to idle, ready for next */
            cmd->command = CMD_IDLE;
        }

        if (cmd->command == CMD_TRANSFER_PARALLEL) {
            /* Execute the 4-lane parallel write (DACs) */
            execute_parallel_transfer();

            /* Reset command to idle, ready for next */
            cmd->command = CMD_IDLE;
        }
    }
}
