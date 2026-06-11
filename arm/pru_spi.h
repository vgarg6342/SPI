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
 * Configuration Functions
 * ----------------------------------------------------------------------- */

/**
 * Set SPI clock mode.
 *
 * Mode | CPOL | CPHA | Description
 * -----|------|------|------------------------------------------
 *  0   |  0   |  0   | Idle LOW,  sample on RISING  edge (default)
 *  1   |  0   |  1   | Idle LOW,  sample on FALLING edge
 *  2   |  1   |  0   | Idle HIGH, sample on FALLING edge
 *  3   |  1   |  1   | Idle HIGH, sample on RISING  edge
 *
 * @param mode  SPI mode (0-3)
 * @return PRU_SPI_OK on success, PRU_SPI_ERR_PARAM if mode > 3
 */
int pru_spi_set_mode(uint8_t mode);

/**
 * Set SPI clock speed.
 *
 * The actual achievable speed depends on the PRU clock (200MHz) and
 * the bit-bang loop overhead. Maximum is ~12.5 MHz, minimum ~250 KHz.
 *
 * @param hz  Target clock frequency in Hz
 * @return    Actual achievable frequency in Hz (closest match)
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
