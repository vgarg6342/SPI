/*
 * pru_spi_fw.c — PRU0 firmware: single-DAC SPI-mode-0 player
 *
 * Runs on PRU0 of the BeagleBone Black (AM335x). One job, no legacy paths:
 *
 *   On CMD_PLAY the PRU
 *     1. copies the framed 16-bit samples from a DDR buffer into its own local
 *        SRAM, in chunks, over the OCP master port (the DDR copy is done here,
 *        on the PRU);
 *     2. shifts each 16-bit word out MSB-first to one SPI DAC on mode 0
 *        (CPOL=0, CPHA=0: idle low, sample on the rising edge), framing every
 *        word with the active-low CS/SYNC line;
 *     3. paces one sample every DAC_SAMPLE_PERIOD_CYCLES using __delay_cycles()
 *        — the sample rate is fixed at compile time; NO IEP timer is used
 *        (pru_iep.h is not available on the target);
 *     4. bumps cmd->heartbeat every sample so the ARM host can tell a busy PRU
 *        apart from a crashed/hung one (the heartbeat does not depend on IEP).
 *
 * Compiled on-device with clpru (TI PRU C compiler).
 *
 * Copyright (c) 2026 — MIT License
 */

#include <stdint.h>
#include <pru_cfg.h>
#include "resource_table.h"
#include "pru_spi_common.h"

/* PRU direct I/O: __R30 = output pins, __R31 = input pins (unused here). */
volatile register uint32_t __R30;
volatile register uint32_t __R31;

/* PRU0 sees its shared RAM at this address; the command block sits at its start. */
#define SHMEM_BASE  0x00010000u

volatile struct pru_dac_cmd *cmd =
    (volatile struct pru_dac_cmd *)(SHMEM_BASE + CMD_BLOCK_SHMEM_OFFSET);

/* Local SRAM landing zone for the DDR copy (PRU0 data RAM starts at 0x0). */
static uint16_t chunk_buf[SRAM_CHUNK_SAMPLES];

/* Compile-time guard: command block is exactly 64 bytes. */
PRU_DAC_CMD_SIZE_CHECK;

/* -----------------------------------------------------------------------
 * Copy 'n' 16-bit samples from a DDR physical address into local SRAM.
 * Done word-at-a-time over the OCP master port — this is the "DDR copy on PRU".
 * ----------------------------------------------------------------------- */
static void ddr_to_local(uint16_t *dst, uint32_t ddr_addr, uint32_t n)
{
    volatile uint32_t *src32 = (volatile uint32_t *)ddr_addr;
    uint32_t *dst32 = (uint32_t *)dst;
    uint32_t pairs = n >> 1;          /* two uint16 per 32-bit read */
    uint32_t i;

    for (i = 0; i < pairs; i++)
        dst32[i] = src32[i];

    if (n & 1u)                       /* trailing odd sample */
        dst[n - 1] = ((volatile uint16_t *)ddr_addr)[n - 1];
}

/* -----------------------------------------------------------------------
 * Idle / park the pins: CS high (deasserted), SDI low, SCLK low (mode 0 idle).
 * ----------------------------------------------------------------------- */
static inline void pins_idle(void)
{
    __R30 = (__R30 & ~(SCLK_BIT | SDI_BIT)) | CS_BIT;
}

/* -----------------------------------------------------------------------
 * Shift ONE 16-bit word out to the DAC, MSB first, SPI mode 0, CS-framed.
 *   CS low -> 16 bits (set SDI, rising edge samples, falling edge shifts) ->
 *   CS high (latches the DAC output, with LDAC tied low).
 * ----------------------------------------------------------------------- */
static inline void emit_frame(uint16_t w)
{
    int k;

    __R30 &= ~(SDI_BIT | SCLK_BIT);       /* SDI low, clock idle low */
    __R30 &= ~CS_BIT;                     /* assert CS (active low)  */
    __delay_cycles(CS_SETUP_CYCLES);

    for (k = DAC_BITS - 1; k >= 0; k--) {
        if (w & (1u << k))
            __R30 |= SDI_BIT;
        else
            __R30 &= ~SDI_BIT;

        __delay_cycles(SCLK_DELAY_CYCLES);
        __R30 |= SCLK_BIT;                /* rising edge: DAC samples SDI */
        __delay_cycles(SCLK_DELAY_CYCLES);
        __R30 &= ~SCLK_BIT;               /* falling edge */
    }

    __delay_cycles(CS_HOLD_CYCLES);
    __R30 |= CS_BIT;                      /* CS high: latch the new output */
}

/* -----------------------------------------------------------------------
 * Execute one CMD_PLAY: copy DDR -> local in chunks and shift out, with the
 * sample rate paced by __delay_cycles() (compile-time fixed; no IEP timer).
 * ----------------------------------------------------------------------- */
static void execute_play(void)
{
    uint32_t ddr_addr, num_samples;
    uint32_t done;

    cmd->status      = STATUS_PLAYING;
    cmd->error_code  = ERR_NONE;
    cmd->samples_done = 0;

    if (cmd->magic != PRU_DAC_MAGIC) {
        cmd->error_code = ERR_BAD_MAGIC;
        cmd->status = STATUS_ERROR;
        return;
    }
    if (cmd->tx_ddr_addr == 0) {
        cmd->error_code = ERR_ADDR_NULL;
        cmd->status = STATUS_ERROR;
        return;
    }
    if (cmd->num_samples == 0) {
        cmd->error_code = ERR_ZERO_LEN;
        cmd->status = STATUS_ERROR;
        return;
    }

    ddr_addr    = cmd->tx_ddr_addr;
    num_samples = cmd->num_samples;

    pins_idle();
    done = 0;

    while (done < num_samples) {
        uint32_t chunk = num_samples - done;
        uint32_t i;

        if (chunk > SRAM_CHUNK_SAMPLES)
            chunk = SRAM_CHUNK_SAMPLES;

        /* PRU pulls the next chunk of samples out of DDR into local SRAM. */
        ddr_to_local(chunk_buf, ddr_addr + (done * 2u), chunk);

        for (i = 0; i < chunk; i++) {
            emit_frame(chunk_buf[i]);

            /* Inter-sample pacing: target period minus the time just spent
             * shifting the frame, so the rate stays ~DAC_SAMPLE_RATE_HZ.
             * __delay_cycles() needs a compile-time constant. */
            __delay_cycles(DAC_SAMPLE_DELAY_CYCLES);

            done++;
            cmd->samples_done = done;
            cmd->heartbeat++;            /* liveness signal for the ARM watchdog */

            if (cmd->command == CMD_STOP)
                goto finished;
        }
    }

finished:
    pins_idle();
    cmd->status = STATUS_DONE;
}

/* -----------------------------------------------------------------------
 * Main: announce readiness, then poll the command block forever.
 * ----------------------------------------------------------------------- */
void main(void)
{
    /* Enable the OCP master port so the PRU can read the DDR sample buffer. */
    CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

    pins_idle();

    cmd->command      = CMD_IDLE;
    cmd->status       = STATUS_IDLE;
    cmd->error_code   = ERR_NONE;
    cmd->heartbeat    = 0;
    cmd->samples_done = 0;
    cmd->late_count   = 0;
    cmd->magic        = PRU_DAC_MAGIC;   /* last: tells ARM we are ready */

    for (;;) {
        uint32_t command = cmd->command;

        cmd->heartbeat++;                /* alive even while idle */

        if (command == CMD_PLAY) {
            execute_play();
            cmd->command = CMD_IDLE;
        } else if (command == CMD_SHUTDOWN) {
            pins_idle();
            cmd->status = STATUS_DONE;
            cmd->command = CMD_IDLE;
            __halt();
            return;
        } else if (command != CMD_IDLE) {
            cmd->command = CMD_IDLE;     /* ignore anything unrecognised */
        }
    }
}
