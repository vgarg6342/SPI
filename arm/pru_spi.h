/*
 * pru_spi.h — ARM-side API for the PRU single-DAC player
 *
 * Minimal interface: bring the PRU up, hand it a table of 12-bit DAC codes to
 * play out of DDR at a fixed sample rate, and find out if the PRU is alive.
 * SPI mode and bit clock are compile-time constants (see pru_spi_common.h), so
 * there are no runtime mode/speed knobs.
 *
 * Typical use:
 *     pru_dac_init();
 *     pru_dac_play(codes, n, 0);          // blocks until done
 *     pru_dac_close();
 *
 * Requires root (for /dev/mem and the remoteproc sysfs interface).
 *
 * Copyright (c) 2026 — MIT License
 */

#ifndef PRU_SPI_H
#define PRU_SPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes (negative = error). */
#define PRU_DAC_OK              0
#define PRU_DAC_ERR_NOT_INIT   -1   /* call pru_dac_init() first              */
#define PRU_DAC_ERR_MMAP       -2   /* /dev/mem or mmap failed (root? uEnv?)  */
#define PRU_DAC_ERR_FIRMWARE   -3   /* remoteproc could not load/start the PRU*/
#define PRU_DAC_ERR_PARAM      -4   /* bad argument                           */
#define PRU_DAC_ERR_BUSY       -5   /* a playback is already running          */
#define PRU_DAC_ERR_TIMEOUT    -6   /* playback did not finish in time        */
#define PRU_DAC_ERR_PRU        -7   /* PRU reported STATUS_ERROR              */
#define PRU_DAC_ERR_HUNG       -8   /* PRU heartbeat froze / firmware crashed */
#define PRU_DAC_ERR_TOO_LARGE  -9   /* more samples than the DDR buffer holds */

/**
 * Bring up the PRU DAC subsystem: map PRUSS + the DDR buffer, load and start the
 * PRU0 firmware, and wait for it to report readiness. Call once at startup.
 *
 * @return PRU_DAC_OK or a negative PRU_DAC_ERR_* code.
 */
int pru_dac_init(void);

/**
 * Shut down: ask the PRU to halt, stop remoteproc, unmap memory. Idempotent.
 */
void pru_dac_close(void);

/**
 * Play a table of 12-bit DAC codes. The codes are framed (control bits applied
 * via DAC_FRAME) into the DDR buffer, then the PRU copies them out of DDR and
 * shifts them to the DAC, one sample per period. Blocks until the PRU finishes,
 * crashes, or the timeout elapses.
 *
 * The sample rate is FIXED at compile time (DAC_SAMPLE_RATE_HZ; the PRU paces
 * with __delay_cycles(), which needs a constant) — there is no runtime rate.
 *
 * @param codes         Array of 12-bit codes (0..4095); out-of-range is masked.
 * @param num_samples   Number of codes (1 .. pru_dac_capacity_samples()).
 * @param timeout_ms    Overall ceiling, ms; 0 = derived from the rate + a margin.
 * @return num_samples on success, or a negative PRU_DAC_ERR_* code.
 *         PRU_DAC_ERR_HUNG specifically means the PRU stopped responding
 *         (see pru_dac_get_state() and `dmesg`).
 */
int pru_dac_play(const uint16_t *codes, uint32_t num_samples,
                 uint32_t timeout_ms);

/**
 * Liveness check: returns 1 if the PRU heartbeat advanced (firmware running),
 * 0 if it is frozen (crashed/hung), or a negative error if not initialized.
 * Costs a few milliseconds (it samples the heartbeat twice).
 */
int pru_dac_is_alive(void);

/**
 * Copy the remoteproc state string (e.g. "running", "offline", "crashed") into
 * buf. Useful when pru_dac_play() returns PRU_DAC_ERR_HUNG.
 *
 * @return PRU_DAC_OK on success, negative on error.
 */
int pru_dac_get_state(char *buf, size_t buflen);

/**
 * Maximum number of samples a single pru_dac_play() can carry (DDR capacity =
 * STREAM_MAX_SAMPLES = rate * STREAM_DURATION_SEC).
 */
uint32_t pru_dac_capacity_samples(void);

/**
 * Human-readable message for a PRU_DAC_ERR_* code.
 */
const char *pru_dac_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* PRU_SPI_H */
