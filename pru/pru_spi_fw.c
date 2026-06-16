/*
 * pru_spi_fw.c — PRU0 Firmware: Mode-0 SPI Master + 4-DAC Streaming Engine
 *
 * This firmware runs on PRU0 of the BeagleBone Black AM335x. It bit-bangs SPI
 * using the PRU's direct GPIO registers (R30 output, R31 input). SPI MODE 0
 * ONLY (CPOL=0, CPHA=0) — modes 1/2/3 were removed for a minimal, deterministic
 * hot path.
 *
 * Three command paths:
 *   CMD_TRANSFER          — legacy single-MOSI full-duplex transfer (via DDR)
 *   CMD_TRANSFER_PARALLEL — legacy one-shot 4-lane write (pre-transposed, DDR)
 *   CMD_STREAM_START      — continuous 4-DAC streaming from a shared-RAM ring
 *                           buffer, paced by the IEP timer (NO DDR). This is the
 *                           path used to push 10k samples/s to 4 DACs at 1 MHz.
 *
 * Streaming data flow (the fast path):
 *   1. ARM writes DAC_FRAME()'d 16-bit words (4 per frame) into the shared-RAM
 *      ring and advances 'head'. No DDR copy is ever performed.
 *   2. PRU consumer pops one frame per IEP deadline, transposes the 4 words into
 *      R30 lane bits, and shifts them out on a shared SCLK with a PRU-driven
 *      shared CS framing each 16-bit word (all 4 DACs latch together).
 *   3. The IEP timer paces the loop to an exact, drift-free sample rate.
 *
 * Compiled with: clpru (TI PRU C/C++ Compiler)
 *
 * Copyright (c) 2026 — MIT License
 */

#include <stdint.h>
#include <string.h>
#include <pru_cfg.h>
#include <pru_iep.h>
#include <pru_intc.h>
#include "resource_table.h"
#include "pru_spi_common.h"

/* -----------------------------------------------------------------------
 * PRU Direct I/O Registers
 *   __R30: output register — controls PRU0 output pins
 *   __R31: input  register — reads PRU0 input pins
 * ----------------------------------------------------------------------- */
volatile register uint32_t __R30;
volatile register uint32_t __R31;

/* Chip-select bit lookup (single-MOSI mode) */
static const uint32_t cs_bits[NUM_CS_LINES] = CS_BIT_LOOKUP;

/* -----------------------------------------------------------------------
 * Shared / local memory pointers
 *
 * PRU0 sees its local data RAM at 0x00000000 and PRU shared RAM at 0x00010000.
 * The command block, the stream control block, and the ring data all live in
 * shared RAM at the offsets defined in pru_spi_common.h.
 * ----------------------------------------------------------------------- */
#define SHMEM_BASE  0x00010000

volatile struct pru_spi_cmd *cmd =
    (volatile struct pru_spi_cmd *)(SHMEM_BASE + CMD_BLOCK_SHMEM_OFFSET);

volatile struct pru_dac_stream *strm =
    (volatile struct pru_dac_stream *)(SHMEM_BASE + STREAM_CTRL_SHMEM_OFFSET);

volatile struct pru_dac_frame *ring =
    (volatile struct pru_dac_frame *)(SHMEM_BASE + RING_DATA_SHMEM_OFFSET);

/* Local SRAM staging buffers (used only by the legacy DDR-backed paths) */
static uint8_t *tx_staging = (uint8_t *)SRAM_TX_OFFSET;
static uint8_t *rx_staging = (uint8_t *)SRAM_RX_OFFSET;

/* Compile-time layout guards: control blocks are 64 B and the ring fits. */
PRU_SPI_CMD_SIZE_CHECK;
PRU_DAC_STREAM_SIZE_CHECK;
PRU_RING_FIT_CHECK;

/* -----------------------------------------------------------------------
 * IEP free-running cycle timer (200 MHz, 5 ns/tick)
 *
 * Used to pace the streaming loop to an exact sample period. We only ever take
 * unsigned/signed differences of the counter, so its absolute origin and 2^32
 * wrap do not matter.
 *
 * NOTE (verify on-device): IEP register access requires pru_iep.h from the
 * pru-software-support-package. DEFAULT_INC=1 makes TMR_CNT advance one tick
 * per IEP clock (200 MHz on AM335x).
 * ----------------------------------------------------------------------- */
static inline void iep_timer_init(void)
{
    /* Disable, set default increment of 1, then enable the compare-0-less
     * free-running counter. */
    CT_IEP.TMR_GLB_CFG_bit.CNT_ENABLE = 0;
    CT_IEP.TMR_CNT = 0;
    CT_IEP.TMR_GLB_CFG_bit.DEFAULT_INC = 1;
    CT_IEP.TMR_GLB_CFG_bit.CNT_ENABLE = 1;
}

static inline uint32_t iep_now(void)
{
    return CT_IEP.TMR_CNT;
}

/* -----------------------------------------------------------------------
 * DDR <-> local SRAM copy (legacy paths only; the stream path never calls these)
 * ----------------------------------------------------------------------- */
static void ddr_to_local(uint8_t *local_dst, uint32_t ddr_src_addr, uint32_t len)
{
    volatile uint8_t *src = (volatile uint8_t *)ddr_src_addr;
    uint32_t i;
    uint32_t words = len >> 2;
    uint32_t remainder = len & 3;
    volatile uint32_t *src32 = (volatile uint32_t *)src;
    uint32_t *dst32 = (uint32_t *)local_dst;
    uint32_t byte_offset;

    for (i = 0; i < words; i++)
        dst32[i] = src32[i];

    byte_offset = words << 2;
    for (i = 0; i < remainder; i++)
        local_dst[byte_offset + i] = src[byte_offset + i];
}

static void local_to_ddr(uint32_t ddr_dst_addr, uint8_t *local_src, uint32_t len)
{
    volatile uint8_t *dst = (volatile uint8_t *)ddr_dst_addr;
    uint32_t i;
    uint32_t words = len >> 2;
    uint32_t remainder = len & 3;
    volatile uint32_t *dst32 = (volatile uint32_t *)dst;
    uint32_t *src32 = (uint32_t *)local_src;
    uint32_t byte_offset;

    for (i = 0; i < words; i++)
        dst32[i] = src32[i];

    byte_offset = words << 2;
    for (i = 0; i < remainder; i++)
        dst[byte_offset + i] = local_src[byte_offset + i];
}

/* -----------------------------------------------------------------------
 * SPI Mode 0 single-byte transfer (single-MOSI lane)
 *   idle LOW, set MOSI, rising edge samples, falling edge shifts.
 * ----------------------------------------------------------------------- */
static inline uint8_t spi_xfer_byte_mode0(uint8_t tx_byte)
{
    uint8_t rx_byte = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        if (tx_byte & (1 << i))
            __R30 |= MOSI_BIT;
        else
            __R30 &= ~MOSI_BIT;

        __delay_cycles(SCLK_DELAY_CYCLES);

        __R30 |= SCLK_BIT;                 /* rising edge: sample */
        if (__R31 & MISO_BIT)
            rx_byte |= (1 << i);

        __delay_cycles(SCLK_DELAY_CYCLES);

        __R30 &= ~SCLK_BIT;                /* falling edge: shift */
    }

    return rx_byte;
}

static void spi_xfer_chunk(uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        rx_buf[i] = spi_xfer_byte_mode0(tx_buf[i]);
}

/* -----------------------------------------------------------------------
 * Emit ONE 16-bit frame to all 4 DAC lanes at once (mode 0, shared CS)
 *
 * Transposes the four already-DAC_FRAME()'d words into R30 lane bits, MSB first,
 * and clocks them out on the shared SCLK. The PRU drives the shared CS/SYNC
 * (PAR_CS_BIT, active LOW): CS goes LOW for the whole 16-bit word and HIGH after
 * to latch all 4 DACs together. Writing the lane bits as a full R30 store keeps
 * CS=0 (asserted) automatically during the bit loop.
 * ----------------------------------------------------------------------- */
static inline void emit_frame(uint16_t w0, uint16_t w1, uint16_t w2, uint16_t w3)
{
    int k;

    /* Assert CS (low); lanes low, clock idle LOW (mode 0) */
    __R30 = 0;
    __delay_cycles(PAR_CS_SETUP_CYCLES);

    for (k = SPI_FRAME_BITS - 1; k >= 0; k--) {
        uint32_t bits = 0;
        if (w0 & (1u << k)) bits |= PAR_MOSI0_BIT;
        if (w1 & (1u << k)) bits |= PAR_MOSI1_BIT;
        if (w2 & (1u << k)) bits |= PAR_MOSI2_BIT;
        if (w3 & (1u << k)) bits |= PAR_MOSI3_BIT;

        __R30 = bits;                      /* data, SCLK low, CS low */
        __delay_cycles(SCLK_DELAY_CYCLES);
        __R30 = bits | PAR_SCLK_BIT;       /* rising edge: DACs sample */
        __delay_cycles(SCLK_DELAY_CYCLES);
    }

    __R30 = 0;                             /* SCLK back low, lanes low, CS low */
    __delay_cycles(PAR_CS_HOLD_CYCLES);
    __R30 = PAR_CS_BIT;                    /* CS HIGH: latch all 4 DACs */
    __delay_cycles(PAR_CS_GAP_CYCLES);
}

/* -----------------------------------------------------------------------
 * Legacy one-shot parallel frame stream (pre-transposed R30 words in DDR)
 *
 * Mode 0 only. Each word in 'stream' already holds the 4 MOSI lane bits for one
 * bit-time (SCLK/CS bits left 0); the ARM side transposed it. 'frames' frames of
 * 'fb' words each are clocked out, CS-framed per frame.
 * ----------------------------------------------------------------------- */
static void spi_xfer_parallel_frames(uint32_t *stream, uint32_t frames, uint32_t fb)
{
    uint32_t f, k, base, w;

    w = 0;
    for (f = 0; f < frames; f++) {
        base = f * fb;
        __R30 = stream[base];              /* assert CS (low), present first bit */
        __delay_cycles(PAR_CS_SETUP_CYCLES);
        for (k = 0; k < fb; k++) {
            w = stream[base + k];
            __R30 = w;                     /* data, SCLK low, CS low */
            __delay_cycles(SCLK_DELAY_CYCLES);
            __R30 = w | PAR_SCLK_BIT;      /* rising edge */
            __delay_cycles(SCLK_DELAY_CYCLES);
        }
        __R30 = w;                         /* clock back to idle LOW */
        __delay_cycles(PAR_CS_HOLD_CYCLES);
        __R30 = PAR_CS_BIT;                /* CS HIGH: latch */
        __delay_cycles(PAR_CS_GAP_CYCLES);
    }
}

/* -----------------------------------------------------------------------
 * Chip-select helpers (single-MOSI mode)
 * ----------------------------------------------------------------------- */
static inline void cs_assert(uint32_t cs_line)    { __R30 &= ~cs_bits[cs_line]; }
static inline void cs_deassert(uint32_t cs_line)  { __R30 |= cs_bits[cs_line]; }
static inline void cs_deassert_all(void)          { __R30 |= CS_ALL_BITS; }

/* -----------------------------------------------------------------------
 * Initialize pins to a safe idle state (mode 0)
 *   - all single-MOSI CS lines HIGH (deasserted)
 *   - SCLK LOW (mode 0 idle)
 *   - all MOSI lanes LOW
 *   - parallel shared CS HIGH (deasserted)
 * ----------------------------------------------------------------------- */
static void spi_init_pins(void)
{
    __R30 |= CS_ALL_BITS;
    __R30 &= ~SCLK_BIT;
    __R30 &= ~(MOSI_BIT | PAR_MOSI_ALL);
    __R30 |= PAR_CS_BIT;
}

/* -----------------------------------------------------------------------
 * Validate the legacy command block (mode 0 only)
 * ----------------------------------------------------------------------- */
static uint32_t validate_command(void)
{
    if (cmd->magic != PRU_SPI_MAGIC)
        return ERR_BAD_MAGIC;
    if (cmd->cs_line >= NUM_CS_LINES)
        return ERR_INVALID_CS;
    if (cmd->spi_mode != SPI_MODE_0)
        return ERR_INVALID_MODE;
    if (cmd->transfer_len == 0)
        return ERR_ZERO_LENGTH;

    return ERR_NONE;
}

/* -----------------------------------------------------------------------
 * Legacy single-MOSI full-duplex transfer (mode 0, DDR-backed)
 * ----------------------------------------------------------------------- */
static void execute_transfer(void)
{
    uint32_t err;
    uint32_t total_len, bytes_remaining, chunk_size, offset;
    uint32_t cs_line, tx_addr, rx_addr;

    cmd->status = STATUS_BUSY;
    cmd->bytes_done = 0;
    cmd->error_code = ERR_NONE;

    err = validate_command();
    if (err != ERR_NONE) {
        cmd->error_code = err;
        cmd->status = STATUS_ERROR;
        return;
    }

    total_len = cmd->transfer_len;
    cs_line   = cmd->cs_line;
    tx_addr   = cmd->tx_ddr_addr;
    rx_addr   = cmd->rx_ddr_addr;

    __R30 &= ~SCLK_BIT;                    /* mode 0: idle LOW */
    __delay_cycles(10);
    cs_assert(cs_line);
    __delay_cycles(20);

    bytes_remaining = total_len;
    offset = 0;
    while (bytes_remaining > 0) {
        chunk_size = bytes_remaining;
        if (chunk_size > SRAM_BUF_SIZE)
            chunk_size = SRAM_BUF_SIZE;

        if (tx_addr != 0) {
            ddr_to_local(tx_staging, tx_addr + offset, chunk_size);
        } else {
            uint32_t k;
            for (k = 0; k < chunk_size; k++)
                tx_staging[k] = 0x00;
        }

        spi_xfer_chunk(tx_staging, rx_staging, chunk_size);

        if (rx_addr != 0)
            local_to_ddr(rx_addr + offset, rx_staging, chunk_size);

        offset += chunk_size;
        bytes_remaining -= chunk_size;
        cmd->bytes_done = offset;
    }

    __delay_cycles(20);
    cs_deassert(cs_line);
    __R30 &= ~SCLK_BIT;
    cmd->status = STATUS_DONE;
}

/* -----------------------------------------------------------------------
 * Legacy one-shot parallel write (pre-transposed R30 stream in DDR, mode 0)
 * ----------------------------------------------------------------------- */
static void execute_parallel_transfer(void)
{
    uint32_t num_frames, frame_bits, tx_addr;
    uint32_t frames_per_chunk, frames_remaining, frame_offset;
    uint32_t *stream = (uint32_t *)tx_staging;

    cmd->status = STATUS_BUSY;
    cmd->bytes_done = 0;
    cmd->error_code = ERR_NONE;

    if (cmd->magic != PRU_SPI_MAGIC) {
        cmd->error_code = ERR_BAD_MAGIC;
        cmd->status = STATUS_ERROR;
        return;
    }
    if (cmd->spi_mode != SPI_MODE_0) {
        cmd->error_code = ERR_INVALID_MODE;
        cmd->status = STATUS_ERROR;
        return;
    }
    if (cmd->tx_ddr_addr == 0) {
        cmd->error_code = ERR_ADDR_NULL;
        cmd->status = STATUS_ERROR;
        return;
    }

    num_frames = cmd->num_frames;
    frame_bits = cmd->frame_bits;
    tx_addr    = cmd->tx_ddr_addr;

    if (num_frames == 0 || frame_bits == 0 || frame_bits > 32) {
        cmd->error_code = ERR_INVALID_FRAMES;
        cmd->status = STATUS_ERROR;
        return;
    }

    frames_per_chunk = (SRAM_BUF_SIZE / 4) / frame_bits;
    if (frames_per_chunk == 0)
        frames_per_chunk = 1;

    __R30 &= ~(PAR_MOSI_ALL | SCLK_BIT);
    __R30 |= PAR_CS_BIT;
    __delay_cycles(10);

    frames_remaining = num_frames;
    frame_offset = 0;
    while (frames_remaining > 0) {
        uint32_t frames_now = frames_remaining;
        uint32_t words_now;

        if (frames_now > frames_per_chunk)
            frames_now = frames_per_chunk;
        words_now = frames_now * frame_bits;

        ddr_to_local(tx_staging,
                     tx_addr + ((frame_offset * frame_bits) << 2),
                     words_now << 2);

        spi_xfer_parallel_frames(stream, frames_now, frame_bits);

        frame_offset += frames_now;
        frames_remaining -= frames_now;
        cmd->bytes_done = (frame_offset * frame_bits) << 2;
    }

    __R30 |= PAR_CS_BIT;
    cmd->status = STATUS_DONE;
}

/* -----------------------------------------------------------------------
 * Continuous 4-DAC streaming from the shared-RAM ring buffer (mode 0)
 *
 * One frame (4 DAC_FRAME()'d words) is popped and shifted out per IEP deadline,
 * giving an exact, drift-free sample rate with NO DDR access. Coordination with
 * the ARM producer is a lock-free SPSC ring:
 *   - occupancy = (uint32_t)(head - tail)
 *   - empty when occupancy == 0
 * Underflow (ring empty at a deadline, producer not finished) is non-fatal: we
 * re-emit the last frame (DAC holds its value) and count it. End-of-stream is
 * signalled by strm->eos; we drain remaining frames then finish.
 * ----------------------------------------------------------------------- */
static void execute_stream(void)
{
    uint32_t period;
    uint32_t deadline;
    uint16_t last0, last1, last2, last3;

    cmd->error_code = ERR_NONE;
    strm->underflow_count = 0;
    strm->frames_played = 0;
    /* head/tail were both reset to 0 by the ARM side before CMD_STREAM_START;
     * do NOT touch tail here — the producer may already be advancing head. */

    period = strm->sample_period_cycles;
    if (period == 0)
        period = DAC_SAMPLE_PERIOD_CYCLES;

    /* Idle the lanes; CS high; clock low (mode 0) */
    __R30 &= ~(PAR_MOSI_ALL | SCLK_BIT);
    __R30 |= PAR_CS_BIT;

    /* Hold value used before the first real frame / on underflow */
    last0 = last1 = last2 = last3 = DAC_FRAME(0);

    cmd->status = STATUS_STREAMING;

    iep_timer_init();
    deadline = iep_now() + period;

    for (;;) {
        uint32_t head = strm->head;
        uint32_t tail = strm->tail;

        if ((uint32_t)(head - tail) != 0) {
            uint32_t idx = tail & RING_INDEX_MASK;
            uint16_t w0 = ring[idx].w[0];
            uint16_t w1 = ring[idx].w[1];
            uint16_t w2 = ring[idx].w[2];
            uint16_t w3 = ring[idx].w[3];

            strm->tail = tail + 1;         /* release the slot to the producer */
            last0 = w0; last1 = w1; last2 = w2; last3 = w3;

            emit_frame(w0, w1, w2, w3);
            strm->frames_played = strm->frames_played + 1;
        } else if (strm->eos) {
            break;                          /* producer done and ring drained */
        } else {
            /* Underflow: hold the last sample so the DAC output does not glitch */
            strm->underflow_count = strm->underflow_count + 1;
            emit_frame(last0, last1, last2, last3);
        }

        if (cmd->command == CMD_STREAM_STOP)
            break;

        /* Pace to the next sample deadline (signed diff handles 32-bit wrap) */
        while ((int32_t)(iep_now() - deadline) < 0) {
            /* spin */
        }
        deadline += period;
    }

    /* Park: CS high, lanes + clock low */
    __R30 &= ~(PAR_MOSI_ALL | SCLK_BIT);
    __R30 |= PAR_CS_BIT;

    if (strm->underflow_count != 0)
        cmd->error_code = ERR_UNDERFLOW;   /* advisory; frames still played */
    cmd->status = STATUS_DONE;
}

/* -----------------------------------------------------------------------
 * Main Entry Point
 * ----------------------------------------------------------------------- */
void main(void)
{
    /* Enable the OCP master port so legacy paths can reach DDR. */
    CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

    spi_init_pins();

    /* Initialize the command block — magic tells the ARM side we are ready. */
    cmd->magic = PRU_SPI_MAGIC;
    cmd->command = CMD_IDLE;
    cmd->status = STATUS_IDLE;
    cmd->bytes_done = 0;
    cmd->error_code = ERR_NONE;
    cmd->spi_mode = SPI_MODE_0;

    /* Initialize the stream control block to a safe, empty state. */
    strm->magic = PRU_SPI_MAGIC;
    strm->head = 0;
    strm->tail = 0;
    strm->capacity = RING_CAPACITY_FRAMES;
    strm->sample_period_cycles = DAC_SAMPLE_PERIOD_CYCLES;
    strm->eos = 0;
    strm->underflow_count = 0;
    strm->frames_played = 0;

    /* Poll for commands from the ARM host. */
    while (1) {
        uint32_t command = cmd->command;

        if (command == CMD_IDLE)
            continue;

        if (command == CMD_SHUTDOWN) {
            cs_deassert_all();
            __R30 &= ~(SCLK_BIT | MOSI_BIT | PAR_MOSI_ALL);
            __R30 |= PAR_CS_BIT;
            cmd->status = STATUS_DONE;
            cmd->command = CMD_IDLE;
            __halt();
            return;
        }

        if (command == CMD_TRANSFER) {
            execute_transfer();
            cmd->command = CMD_IDLE;
        } else if (command == CMD_TRANSFER_PARALLEL) {
            execute_parallel_transfer();
            cmd->command = CMD_IDLE;
        } else if (command == CMD_STREAM_START) {
            execute_stream();
            cmd->command = CMD_IDLE;
        } else {
            /* Unknown command — ignore and clear */
            cmd->command = CMD_IDLE;
        }
    }
}
