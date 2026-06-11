/*
 * pru_spi_common.h — Shared definitions for PRU SPI bit-bang
 *
 * This header is included by BOTH the PRU firmware (compiled with clpru)
 * and the ARM host code (compiled with gcc). Keep it C89-compatible
 * and free of platform-specific includes.
 *
 * Copyright (c) 2026 — MIT License
 */

#ifndef PRU_SPI_COMMON_H
#define PRU_SPI_COMMON_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Magic number for command block validation
 * ----------------------------------------------------------------------- */
#define PRU_SPI_MAGIC           0x53504900  /* "SPI\0" in ASCII */

/* -----------------------------------------------------------------------
 * Commands (ARM → PRU)
 * ----------------------------------------------------------------------- */
#define CMD_IDLE                0   /* No operation, PRU is idle */
#define CMD_TRANSFER            1   /* Execute SPI transfer */
#define CMD_SHUTDOWN            2   /* Gracefully stop PRU firmware */

/* -----------------------------------------------------------------------
 * Status (PRU → ARM)
 * ----------------------------------------------------------------------- */
#define STATUS_IDLE             0   /* PRU is idle, ready for command */
#define STATUS_BUSY             1   /* Transfer in progress */
#define STATUS_DONE             2   /* Transfer completed successfully */
#define STATUS_ERROR            3   /* Transfer failed */

/* -----------------------------------------------------------------------
 * Error codes
 * ----------------------------------------------------------------------- */
#define ERR_NONE                0
#define ERR_BAD_MAGIC           1   /* Command block magic mismatch */
#define ERR_INVALID_CS          2   /* CS line out of range (0-3) */
#define ERR_INVALID_MODE        3   /* SPI mode out of range (0-3) */
#define ERR_ZERO_LENGTH         4   /* Transfer length is 0 */
#define ERR_ADDR_NULL           5   /* TX or RX DDR address is NULL */

/* -----------------------------------------------------------------------
 * SPI Modes (CPOL | CPHA)
 * ----------------------------------------------------------------------- */
#define SPI_MODE_0              0   /* CPOL=0, CPHA=0: idle low, sample on rising  */
#define SPI_MODE_1              1   /* CPOL=0, CPHA=1: idle low, sample on falling */
#define SPI_MODE_2              2   /* CPOL=1, CPHA=0: idle high, sample on falling*/
#define SPI_MODE_3              3   /* CPOL=1, CPHA=1: idle high, sample on rising */

/* -----------------------------------------------------------------------
 * PRU Pin Assignments (PRU0 Enhanced GPIO via R30/R31)
 *
 * Signal  | PRU0 Bit | Header Pin | Direction
 * --------|----------|------------|----------
 * SCLK    | bit 0    | P9_31      | Output (R30)
 * MOSI    | bit 1    | P9_29      | Output (R30)
 * MISO    | bit 2    | P9_30      | Input  (R31)
 * CS0     | bit 3    | P9_28      | Output (R30)
 * CS1     | bit 5    | P9_27      | Output (R30)
 * CS2     | bit 7    | P9_25      | Output (R30)
 * CS3     | bit 4    | P9_42      | Output (R30)
 * ----------------------------------------------------------------------- */
#define SCLK_BIT                (1 << 0)    /* 0x0001 */
#define MOSI_BIT                (1 << 1)    /* 0x0002 */
#define MISO_BIT                (1 << 2)    /* 0x0004 */
#define CS0_BIT                 (1 << 3)    /* 0x0008 */
#define CS3_BIT                 (1 << 4)    /* 0x0010 */
#define CS1_BIT                 (1 << 5)    /* 0x0020 */
#define CS2_BIT                 (1 << 7)    /* 0x0080 */

/* All CS lines OR'd together */
#define CS_ALL_BITS             (CS0_BIT | CS1_BIT | CS2_BIT | CS3_BIT)

/* Number of chip select lines */
#define NUM_CS_LINES            4

/* CS bit lookup (indexed by cs_line 0-3) */
#define CS_BIT_LOOKUP           { CS0_BIT, CS1_BIT, CS2_BIT, CS3_BIT }

/* -----------------------------------------------------------------------
 * Timing
 *
 * PRU runs at 200MHz (5ns per cycle).
 * For 10MHz SPI clock: period = 100ns, half-period = 50ns = 10 cycles.
 * The bit-bang loop body uses ~6 cycles, leaving ~4 for __delay_cycles.
 * Adjust DEFAULT_CLOCK_DIV to tune actual frequency.
 * ----------------------------------------------------------------------- */
#define PRU_CLK_HZ              200000000   /* 200 MHz */
#define DEFAULT_SPI_HZ          10000000    /* 10 MHz target */
#define DEFAULT_CLOCK_DIV       4           /* delay cycles per half-period */
#define MIN_CLOCK_DIV           1           /* ~12.5 MHz max with loop overhead */
#define MAX_CLOCK_DIV           200         /* ~250 KHz minimum */

/* -----------------------------------------------------------------------
 * Memory Map
 * ----------------------------------------------------------------------- */

/* PRUSS base physical address */
#define PRUSS_BASE_ADDR         0x4A300000

/* PRU0 local data RAM: 8KB */
#define PRU0_DRAM_OFFSET        0x00000000
#define PRU0_DRAM_SIZE          0x00002000  /* 8192 bytes */

/* PRU1 local data RAM: 8KB */
#define PRU1_DRAM_OFFSET        0x00002000
#define PRU1_DRAM_SIZE          0x00002000

/* PRU Shared RAM: 12KB */
#define PRU_SHAREDMEM_OFFSET    0x00010000
#define PRU_SHAREDMEM_SIZE      0x00003000  /* 12288 bytes */

/* Total PRUSS mapping size (covers DRAM0 + DRAM1 + ... + Shared RAM) */
#define PRUSS_MAP_SIZE          0x00020000  /* 128KB covers everything */

/* -----------------------------------------------------------------------
 * Local SRAM Buffer Layout (PRU0 Data RAM — 8KB)
 *
 * 0x0000 - 0x0FFF : TX staging buffer (4096 bytes)
 * 0x1000 - 0x1FFF : RX staging buffer (4096 bytes)
 * ----------------------------------------------------------------------- */
#define SRAM_TX_OFFSET          0x0000
#define SRAM_RX_OFFSET          0x1000
#define SRAM_BUF_SIZE           4096        /* 4KB per buffer */

/* -----------------------------------------------------------------------
 * DDR Buffer Configuration
 *
 * The ARM side allocates DDR buffers and passes their physical addresses
 * in the command block. Default max transfer size is 64KB but can be
 * increased by allocating larger DDR buffers.
 * ----------------------------------------------------------------------- */
#define DDR_BUF_DEFAULT_SIZE    (64 * 1024)  /* 64KB default */

/* -----------------------------------------------------------------------
 * Command Block Structure
 *
 * Placed in PRU Shared RAM at offset 0x0000 (physical: PRUSS_BASE + 0x10000).
 * Must be exactly 64 bytes and identical between ARM and PRU builds.
 * ----------------------------------------------------------------------- */
struct pru_spi_cmd {
    volatile uint32_t magic;          /* 0x00: Must be PRU_SPI_MAGIC */
    volatile uint32_t command;        /* 0x04: CMD_IDLE / CMD_TRANSFER / CMD_SHUTDOWN */
    volatile uint32_t cs_line;        /* 0x08: Chip select index 0-3 */
    volatile uint32_t spi_mode;       /* 0x0C: SPI mode 0-3 */
    volatile uint32_t clock_div;      /* 0x10: delay cycles per half-period */
    volatile uint32_t tx_ddr_addr;    /* 0x14: Physical DDR address of TX data */
    volatile uint32_t rx_ddr_addr;    /* 0x18: Physical DDR address of RX data */
    volatile uint32_t transfer_len;   /* 0x1C: Number of bytes to transfer */
    volatile uint32_t status;         /* 0x20: STATUS_IDLE/BUSY/DONE/ERROR */
    volatile uint32_t bytes_done;     /* 0x24: Transfer progress in bytes */
    volatile uint32_t error_code;     /* 0x28: Error detail when status==ERROR */
    volatile uint32_t reserved[5];    /* 0x2C-0x3F: Pad to 64 bytes total */
};

/* Compile-time check: ensure struct is exactly 64 bytes */
#define PRU_SPI_CMD_SIZE_CHECK \
    typedef char _pru_spi_cmd_size_check \
        [(sizeof(struct pru_spi_cmd) == 64) ? 1 : -1]

/* -----------------------------------------------------------------------
 * Remoteproc paths
 * ----------------------------------------------------------------------- */
#define REMOTEPROC_PRU0_PATH    "/sys/class/remoteproc/remoteproc1"
#define PRU_FW_NAME             "am335x-pru0-fw"
#define PRU_FW_INSTALL_PATH     "/lib/firmware/" PRU_FW_NAME

#endif /* PRU_SPI_COMMON_H */
