/*
 * pru_spi.h — ARM-side API for PRU Bit-Banged SPI Master
 *
 * This library provides a clean C interface for controlling the PRU0
 * SPI bit-bang engine on BeagleBone Black. It handles:
 *   - PRU firmware loading via remoteproc
 *   - Shared memory setup and communication
 *   - DDR buffer management for TX/RX data
 *   - SPI mode and clock configuration
 *
 * Usage:
 *   1. Call pru_spi_init() once at startup
 *   2. Use pru_spi_transfer() / pru_spi_write() / pru_spi_read()
 *   3. Call pru_spi_close() at shutdown
 *
 * Requires: root privileges (for /dev/mem and remoteproc access)
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

/* -----------------------------------------------------------------------
 * Error Codes (negative values)
 * ----------------------------------------------------------------------- */
#define PRU_SPI_OK               0
#define PRU_SPI_ERR_INIT        -1   /* Initialization failed */
#define PRU_SPI_ERR_MMAP        -2   /* Memory mapping failed */
#define PRU_SPI_ERR_FIRMWARE    -3   /* Firmware loading failed */
#define PRU_SPI_ERR_TIMEOUT     -4   /* Transfer timed out */
#define PRU_SPI_ERR_PARAM       -5   /* Invalid parameter */
#define PRU_SPI_ERR_NOT_INIT    -6   /* Library not initialized */
#define PRU_SPI_ERR_BUSY        -7   /* PRU is busy with another transfer */
#define PRU_SPI_ERR_PRU         -8   /* PRU reported an error */
#define PRU_SPI_ERR_ALLOC       -9   /* Buffer allocation failed */

/* -----------------------------------------------------------------------
 * Configuration Defaults
 * ----------------------------------------------------------------------- */
#define PRU_SPI_DEFAULT_TIMEOUT_MS  5000    /* 5 second default timeout */
#define PRU_SPI_MAX_CS              4       /* Number of chip select lines */
#define PRU_SPI_MAX_SPEED_HZ        12500000 /* ~12.5 MHz theoretical max */
#define PRU_SPI_DEFAULT_SPEED_HZ    10000000 /* 10 MHz default */

/* -----------------------------------------------------------------------
 * Initialization / Shutdown
 * ----------------------------------------------------------------------- */

/**
 * Initialize the PRU SPI subsystem.
 *
 * This function:
 *   1. Opens /dev/mem and maps PRUSS shared memory
 *   2. Allocates DDR buffers for TX/RX data
 *   3. Loads the PRU0 firmware via remoteproc
 *   4. Waits for the PRU to signal readiness
 *
 * Must be called before any other pru_spi_* function.
 * Requires root privileges.
 *
 * @return PRU_SPI_OK on success, negative error code on failure
 */
int pru_spi_init(void);

/**
 * Shutdown the PRU SPI subsystem.
 *
 * Sends a shutdown command to the PRU, stops remoteproc,
 * and unmaps all memory. Safe to call multiple times.
 */
void pru_spi_close(void);

/* -----------------------------------------------------------------------
 * SPI Transfer Functions
 * ----------------------------------------------------------------------- */

/**
 * Full-duplex SPI transfer.
 *
 * Simultaneously transmits from tx_buf and receives into rx_buf.
 * Both buffers must be at least 'len' bytes.
 *
 * @param cs         Chip select index (0-3)
 * @param tx_buf     Data to transmit. If NULL, sends 0x00 for each byte.
 * @param rx_buf     Buffer for received data. If NULL, received data is discarded.
 * @param len        Number of bytes to transfer (must be > 0)
 * @param timeout_ms Timeout in milliseconds. 0 = use default (5s).
 *
 * @return Number of bytes transferred on success, negative error code on failure
 */
int pru_spi_transfer(uint8_t cs, const uint8_t *tx_buf,
                     uint8_t *rx_buf, uint32_t len,
                     uint32_t timeout_ms);

/**
 * Write-only SPI transfer.
 *
 * Transmits data from tx_buf. Received data (MISO) is discarded.
 * Convenience wrapper around pru_spi_transfer().
 *
 * @param cs         Chip select index (0-3)
 * @param tx_buf     Data to transmit (must not be NULL)
 * @param len        Number of bytes to transmit
 * @param timeout_ms Timeout in milliseconds (0 = default)
 *
 * @return Bytes written on success, negative error code on failure
 */
int pru_spi_write(uint8_t cs, const uint8_t *tx_buf,
                  uint32_t len, uint32_t timeout_ms);

/**
 * Read-only SPI transfer.
 *
 * Sends 0x00 while reading data into rx_buf.
 * Convenience wrapper around pru_spi_transfer().
 *
 * @param cs         Chip select index (0-3)
 * @param rx_buf     Buffer to store received data (must not be NULL)
 * @param len        Number of bytes to read
 * @param timeout_ms Timeout in milliseconds (0 = default)
 *
 * @return Bytes read on success, negative error code on failure
 */
int pru_spi_read(uint8_t cs, uint8_t *rx_buf,
                 uint32_t len, uint32_t timeout_ms);

/* -----------------------------------------------------------------------
 * Parallel (4-lane) DAC Write
 * ----------------------------------------------------------------------- */

/**
 * Write to 4 DACs SIMULTANEOUSLY, each on its own MOSI lane.
 *
 * All four DACs share one SCLK and one CS/SYNC but receive INDEPENDENT data —
 * DAC0 gets dac0[], DAC1 gets dac1[], etc. Each frame is SPI_FRAME_BITS bits
 * wide (compile-time, default 16). The four streams are bit-transposed and
 * shifted out together: on every clock edge all four lanes advance one bit.
 *
 * Write-only (no MISO). The PRU drives the shared CS/SYNC (active LOW) to frame
 * and latch each word, so all 4 DACs latch together — no ARM GPIO needed.
 * Because CS is shared, every frame updates all 4 DACs at once; you cannot
 * address a single DAC on its own.
 *
 * Any of dac0..dac3 may be NULL, in which case that lane is driven with 0.
 *
 * @param dac0..dac3  Per-DAC data, each 'num_frames' words (or NULL)
 * @param num_frames  Number of frames (words) per DAC (must be > 0)
 * @param timeout_ms  Timeout in milliseconds (0 = default)
 * @return num_frames on success, negative error code on failure
 */
int pru_spi_parallel_write(const uint16_t *dac0, const uint16_t *dac1,
                           const uint16_t *dac2, const uint16_t *dac3,
                           uint32_t num_frames, uint32_t timeout_ms);

/**
 * Convenience: write a single value to each of the 4 DACs at once.
 *
 * @param d0..d3      One word per DAC
 * @param timeout_ms  Timeout in milliseconds (0 = default)
 * @return 1 on success, negative error code on failure
 */
int pru_spi_parallel_write_one(uint16_t d0, uint16_t d1,
                               uint16_t d2, uint16_t d3,
                               uint32_t timeout_ms);

/**
 * Select how many DACs / MOSI lanes are active in parallel mode (runtime).
 *
 * With n active lanes, only MOSI0..MOSI(n-1) carry data; the remaining lanes
 * are held LOW (idle). The data buffers for inactive lanes are ignored, so you
 * can pass NULL (or leftover buffers) for them. Default is all 4.
 *
 * @param n  Number of active DACs/lanes, 1..4
 * @return PRU_SPI_OK on success, PRU_SPI_ERR_PARAM if out of range
 */
int pru_spi_set_num_dacs(uint8_t n);

/**
 * Get the current number of active parallel DACs/lanes.
 *
 * @return 1..4, or negative error if not initialized
 */
int pru_spi_get_num_dacs(void);

/* -----------------------------------------------------------------------
 * Continuous 4-DAC Streaming (shared-RAM ring buffer, no DDR)
 *
 * Push DAC samples to all 4 DACs at a fixed sample rate (default 10 kHz) with
 * the PRU pacing each frame off its IEP timer. Samples are fed through a
 * lock-free ring buffer in PRU shared RAM — the DDR path is never touched.
 *
 * Typical use:
 *   pru_dac_stream_start(10000);
 *   for each sample:  pru_dac_stream_push(frame4);   // frame4 = {d0,d1,d2,d3}
 *   underflows = pru_dac_stream_end(0);
 *
 * Values are raw 12-bit DAC codes (0..4095); DAC_FRAME() control bits are
 * applied internally. The producer applies backpressure: push blocks (without
 * overwriting) while the ring is full, so data is never dropped.
 * ----------------------------------------------------------------------- */

/**
 * Begin a streaming session.
 *
 * Resets the ring, programs the sample period, and tells the PRU to start its
 * IEP-paced consumer loop. Call pru_dac_stream_push() to feed samples.
 *
 * @param sample_rate_hz  Samples per second (e.g. 10000). 0 = compile-time default.
 * @return PRU_SPI_OK on success, negative error code on failure
 */
int pru_dac_stream_start(uint32_t sample_rate_hz);

/**
 * Push one sample (one value per DAC) into the ring.
 *
 * Blocks with backpressure while the ring is full; never overwrites unsent data.
 *
 * @param dac_values  Four raw 12-bit DAC codes (0..4095), DAC0..DAC3.
 * @return PRU_SPI_OK on success, negative error code on failure
 */
int pru_dac_stream_push(const uint16_t dac_values[4]);

/**
 * Finish a streaming session.
 *
 * Signals end-of-stream, waits for the PRU to drain the ring and stop, then
 * returns the number of underflows observed during the session.
 *
 * @param timeout_ms  Max time to wait for drain (0 = default).
 * @return >= 0 underflow count on success, negative error code on failure
 */
int pru_dac_stream_end(uint32_t timeout_ms);

/* -----------------------------------------------------------------------
 * Configuration Functions
 * ----------------------------------------------------------------------- */

/**
 * Set SPI clock mode.
 *
 * This firmware supports MODE 0 ONLY (CPOL=0, CPHA=0: idle LOW, sample on the
 * rising edge). Modes 1/2/3 were removed.
 *
 * @param mode  Must be 0.
 * @return PRU_SPI_OK if mode == 0, PRU_SPI_ERR_PARAM otherwise
 */
int pru_spi_set_mode(uint8_t mode);

/**
 * Set SPI clock speed.
 *
 * DEPRECATED / NO-OP: the SPI clock is fixed at COMPILE TIME via SPI_SCLK_HZ
 * in pru_spi_common.h, because the PRU bit-bang delay uses __delay_cycles(),
 * a compiler intrinsic that requires a constant. To change the clock, edit
 * SPI_SCLK_HZ, rebuild the firmware, and redeploy. This function logs a
 * message and returns the fixed compile-time speed.
 *
 * @param hz  Ignored.
 * @return    The fixed compile-time frequency in Hz (SPI_SCLK_HZ)
 */
uint32_t pru_spi_set_speed(uint32_t hz);

/**
 * Get current SPI mode.
 *
 * @return Current SPI mode (0-3), or negative error if not initialized
 */
int pru_spi_get_mode(void);

/**
 * Get current SPI speed setting.
 *
 * @return Current speed in Hz, or 0 if not initialized
 */
uint32_t pru_spi_get_speed(void);

/**
 * Get the maximum supported transfer size in bytes.
 *
 * @return Maximum single-transfer size in bytes
 */
uint32_t pru_spi_get_max_transfer_size(void);

/**
 * Check if the PRU SPI subsystem is initialized and ready.
 *
 * @return 1 if ready, 0 if not initialized or in error state
 */
int pru_spi_is_ready(void);

/**
 * Get a human-readable error message for an error code.
 *
 * @param err  Error code returned by a pru_spi_* function
 * @return     Static string describing the error
 */
const char *pru_spi_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* PRU_SPI_H */
