/*
 * pru_spi_common.h — Shared ARM/PRU definitions for the single-DAC player
 *
 * One file, included by BOTH:
 *   - the PRU0 firmware  (compiled on-device with clpru)
 *   - the ARM host code  (compiled on-device with gcc)
 *
 * It describes ONE definitive job: stream a table of 12-bit DAC codes out of a
 * DDR buffer to a single SPI DAC (MCP4921-style, 16-bit word = 4 control bits +
 * 12 data bits) at a fixed sample rate, on SPI mode 0.
 *
 * Data flow (this is the whole protocol):
 *   1. ARM frames every 12-bit code into a 16-bit word (DAC_FRAME) and writes the
 *      whole array into a physically-contiguous DDR buffer.
 *   2. ARM hands the PRU the DDR physical address + sample count + sample period
 *      via the command block in PRU shared RAM, then sets command = CMD_PLAY.
 *   3. The PRU copies the samples DDR -> its own local SRAM in chunks (the DDR
 *      copy is done BY THE PRU over its OCP master port) and shifts each 16-bit
 *      word out MSB-first, pacing one sample per IEP-timer deadline.
 *   4. The PRU bumps a heartbeat word every sample so the ARM side can tell a
 *      live-but-busy PRU apart from a crashed/hung one.
 *
 * Keep this header C89-clean and free of platform headers.
 *
 * Copyright (c) 2026 — MIT License
 */

#ifndef PRU_SPI_COMMON_H
#define PRU_SPI_COMMON_H

#include <stdint.h>

/* =======================================================================
 * 1. Handshake constants
 * ======================================================================= */

/* Magic the PRU writes once it has booted and the command block is valid. */
#define PRU_DAC_MAGIC           0x44414331u   /* "DAC1" */

/* Commands: ARM -> PRU (written to cmd->command). */
#define CMD_IDLE                0u   /* nothing to do                         */
#define CMD_PLAY                1u   /* play num_samples words from DDR        */
#define CMD_STOP                2u   /* abort an in-progress playback          */
#define CMD_SHUTDOWN            3u   /* park pins and halt the PRU             */

/* Status: PRU -> ARM (read from cmd->status). */
#define STATUS_IDLE             0u   /* ready for a command                    */
#define STATUS_PLAYING          1u   /* playback in progress                   */
#define STATUS_DONE             2u   /* last playback finished OK              */
#define STATUS_ERROR            3u   /* last command failed (see error_code)   */

/* Error detail (cmd->error_code, valid when status == STATUS_ERROR). */
#define ERR_NONE                0u
#define ERR_BAD_MAGIC           1u   /* command block magic mismatch           */
#define ERR_ADDR_NULL           2u   /* tx_ddr_addr was 0                       */
#define ERR_ZERO_LEN            3u   /* num_samples was 0                       */

/* =======================================================================
 * 2. Pin map — single DAC, SPI mode 0, all on PRU0 R30 (outputs only)
 *
 *   Signal | R30 bit | Header pin | DAC pin
 *   -------|---------|------------|--------------------------
 *   SCLK   | bit 0   | P9_31      | SCK
 *   SDI    | bit 1   | P9_29      | SDI / MOSI / DIN
 *   CS     | bit 3   | P9_28      | CS / SYNC  (active LOW)
 *
 * There is NO MISO: SPI DACs are write-only. Tie the DAC's LDAC pin LOW so the
 * output updates when CS rises at the end of each 16-bit word.
 * ======================================================================= */
#define SCLK_BIT                (1u << 0)     /* P9_31 */
#define SDI_BIT                 (1u << 1)     /* P9_29 */
#define CS_BIT                  (1u << 3)     /* P9_28, active LOW */

/* =======================================================================
 * 3. Timing — everything that __delay_cycles() touches is COMPILE-TIME
 *
 * The PRU runs at 200 MHz (5 ns/cycle). The SPI bit clock is generated with
 * __delay_cycles(), a compiler intrinsic that REQUIRES a constant argument, so
 * the SPI clock is fixed at build time. To change it: edit SPI_SCLK_HZ, rebuild
 * the firmware, redeploy. (This is why there is no runtime "set speed".)
 * ======================================================================= */
#define PRU_CLK_HZ              200000000u    /* 200 MHz */

#define SPI_SCLK_HZ             1000000u      /* <-- EDIT to set SPI bit clock (1 MHz) */
#define SCLK_HALF_CYCLES        (PRU_CLK_HZ / (2u * SPI_SCLK_HZ))
#define SCLK_LOOP_OVERHEAD      3u            /* approx cycles spent in the bit body */
#define SCLK_DELAY_CYCLES       (SCLK_HALF_CYCLES > SCLK_LOOP_OVERHEAD ? \
                                 (SCLK_HALF_CYCLES - SCLK_LOOP_OVERHEAD) : 1u)

/* CS/SYNC framing around each 16-bit word (PRU cycles @ 5 ns). */
#define CS_SETUP_CYCLES         20u           /* CS low  -> first SCLK edge (~100 ns) */
#define CS_HOLD_CYCLES          20u           /* last SCLK edge -> CS high  (~100 ns) */

/* =======================================================================
 * 4. DAC framing — 12-bit code -> 16-bit SPI word (Microchip MCP49xx layout)
 *
 *   bit 15  A/B   channel select   (0 = DAC_A)
 *   bit 14  BUF   input buffer      (0 = unbuffered)
 *   bit 13  GA    output gain       (1 = 1x, 0 = 2x)
 *   bit 12  SHDN  output control    (1 = active / output enabled)
 *   bit 11..0     12-bit data (0..4095)
 *
 * DAC_CTRL_BITS = 0x3000  => DAC_A, unbuffered, 1x gain, output active.
 *
 * >>> EDIT DAC_CTRL_BITS for your exact DAC's control/command bits. <<<
 * ======================================================================= */
#define DAC_BITS                16u           /* SPI word width, bits           */
#define DAC_VALUE_MAX           0x0FFFu       /* 12-bit full scale (4095)        */
#define DAC_CTRL_BITS           0x3000u       /* MCP49xx: DAC_A, 1x gain, active */
#define DAC_FRAME(v)            ((uint16_t)(DAC_CTRL_BITS | \
                                 ((uint16_t)(v) & DAC_VALUE_MAX)))

/* =======================================================================
 * 5. Sample rate and transmission duration (the "control variables")
 *
 * The per-sample cadence is generated on the PRU with __delay_cycles() — there
 * is NO IEP timer (pru_iep.h is not available on the target). Because
 * __delay_cycles() needs a compile-time constant, the sample rate is fixed at
 * BUILD TIME by the macros below; there is no runtime rate.
 *
 *   DAC_SAMPLE_RATE_HZ       — samples per second (10k => 100 us period).
 *   DAC_SAMPLE_PERIOD_CYCLES — the target sample period in PRU cycles (= 1/rate).
 *   FRAME_SHIFT_CYCLES       — (approx) cycles spent shifting one 16-bit frame.
 *   DAC_SAMPLE_DELAY_CYCLES  — what we actually feed __delay_cycles() after each
 *                              frame: the period MINUS the shift time, so the
 *                              real rate stays close to DAC_SAMPLE_RATE_HZ. (For
 *                              the dead-simple "delay = full period" behaviour,
 *                              just use DAC_SAMPLE_PERIOD_CYCLES in the firmware.)
 *   STREAM_DURATION_SEC      — how long a full run lasts.
 *   STREAM_MAX_SAMPLES       — DDR capacity in samples = rate * duration. Sizes
 *                              the DDR buffer and caps a single CMD_PLAY.
 * ======================================================================= */
#define DAC_SAMPLE_RATE_HZ      10000u        /* <-- EDIT: samples/s (100 us period) */
#define DAC_SAMPLE_PERIOD_CYCLES (PRU_CLK_HZ / DAC_SAMPLE_RATE_HZ)  /* 20000 @ 10kHz */

/* Approx cycles to shift one 16-bit frame: CS setup/hold + 16 bits, each bit
 * being two SCLK half-periods plus the bit-body overhead. Used only to keep the
 * sample period accurate; tune SCLK_LOOP_OVERHEAD if the measured rate is off
 * (see USER_MANUAL §8a). */
#define FRAME_SHIFT_CYCLES      (CS_SETUP_CYCLES + CS_HOLD_CYCLES + \
                                 DAC_BITS * (2u * SCLK_DELAY_CYCLES + SCLK_LOOP_OVERHEAD))

/* Inter-frame delay fed to __delay_cycles(): target period minus the shift time. */
#define DAC_SAMPLE_DELAY_CYCLES (DAC_SAMPLE_PERIOD_CYCLES > FRAME_SHIFT_CYCLES ? \
                                 (DAC_SAMPLE_PERIOD_CYCLES - FRAME_SHIFT_CYCLES) : 1u)

#define STREAM_DURATION_SEC     5u            /* <-- EDIT: transmission length, seconds */
#define STREAM_MAX_SAMPLES      (DAC_SAMPLE_RATE_HZ * STREAM_DURATION_SEC)

/* =======================================================================
 * 6. Memory map
 * ======================================================================= */

/* PRUSS registers / RAM (mapped by the ARM side via /dev/mem). */
#define PRUSS_BASE_ADDR         0x4A300000u
#define PRU_SHAREDMEM_OFFSET    0x00010000u   /* shared RAM, from PRUSS base     */
#define PRU_SHAREDMEM_SIZE      0x00003000u   /* 12 KB                           */
#define PRUSS_MAP_SIZE          0x00020000u   /* 128 KB covers everything        */

/* PRU0 local data RAM staging area for the DDR copy (PRU sees it at 0x0). */
#define SRAM_CHUNK_SAMPLES      512u          /* samples copied DDR->local per chunk */
#define SRAM_CHUNK_BYTES        (SRAM_CHUNK_SAMPLES * 2u)   /* 1 KB             */

/* The command block lives at the start of PRU shared RAM. */
#define CMD_BLOCK_SHMEM_OFFSET  0x0000u

/* -----------------------------------------------------------------------
 * DDR sample buffer
 *
 * The ARM side maps a physically-contiguous DDR region via /dev/mem and writes
 * the framed 16-bit samples there; the PRU reads them over its OCP master port.
 *
 * DDR_BUF_PHYS_BASE MUST point at RAM that Linux does not use. The supported way
 * to guarantee that on the BeagleBone Black is to shrink the kernel's view of
 * RAM in uEnv.txt (e.g. "mem=496M" leaves 0x9F000000..0x9FFFFFFF, 16 MB, free).
 * See USER_MANUAL.md §3 (uEnv.txt) before running.
 * ----------------------------------------------------------------------- */
#define DDR_BUF_PHYS_BASE       0x9F000000u   /* top-of-RAM carve-out (reserve!) */
#define DDR_BUF_BYTES           (STREAM_MAX_SAMPLES * 2u)  /* one 16-bit word/sample */

/* =======================================================================
 * 7. Command block — 64 bytes, identical on both sides, in PRU shared RAM
 * ======================================================================= */
struct pru_dac_cmd {
    volatile uint32_t magic;                /* 0x00: PRU_DAC_MAGIC (set by PRU)  */
    volatile uint32_t command;              /* 0x04: CMD_* (ARM writes)          */
    volatile uint32_t status;               /* 0x08: STATUS_* (PRU writes)       */
    volatile uint32_t error_code;           /* 0x0C: ERR_* (PRU writes)          */
    volatile uint32_t heartbeat;            /* 0x10: PRU bumps it; ARM watchdog  */
    volatile uint32_t tx_ddr_addr;          /* 0x14: phys addr of framed samples */
    volatile uint32_t num_samples;          /* 0x18: how many 16-bit words       */
    volatile uint32_t samples_done;         /* 0x1C: PRU progress counter        */
    volatile uint32_t reserved[8];          /* 0x20-0x3F: pad to 64 bytes        */
};

/* Compile-time guard: the command block must be exactly 64 bytes on both sides. */
#define PRU_DAC_CMD_SIZE_CHECK \
    typedef char _pru_dac_cmd_size_check \
        [(sizeof(struct pru_dac_cmd) == 64) ? 1 : -1]

/* =======================================================================
 * 8. Remoteproc paths (ARM side uses these to load/start/stop PRU0)
 * ======================================================================= */
#define REMOTEPROC_PRU0_PATH    "/sys/class/remoteproc/remoteproc1"
#define PRU_FW_NAME             "am335x-pru0-fw"
#define PRU_FW_INSTALL_PATH     "/lib/firmware/" PRU_FW_NAME

#endif /* PRU_SPI_COMMON_H */
