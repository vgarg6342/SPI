/*
 * pru_spi_example.c — Example usage of the PRU SPI bit-bang API
 *
 * Demonstrates:
 *   1. Initialization and configuration
 *   2. SPI write to a specific CS line
 *   3. Full-duplex SPI transfer
 *   4. SPI read-only transfer
 *   5. Loopback test (wire MOSI → MISO)
 *   6. Multi-CS sequential transfers
 *   7. Speed and mode configuration
 *   8. Proper cleanup
 *
 * Usage:
 *   sudo ./pru_spi_example              # Run all demos
 *   sudo ./pru_spi_example --loopback   # Run loopback test (MOSI→MISO)
 *   sudo ./pru_spi_example --write      # Write-only demo
 *   sudo ./pru_spi_example --speed N    # Set speed to N Hz
 *
 * Copyright (c) 2026 — MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>

#include "pru_spi.h"

/* -----------------------------------------------------------------------
 * Signal Handling (Ctrl-C cleanup)
 * ----------------------------------------------------------------------- */
static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    printf("\nCaught signal, shutting down...\n");
    g_running = 0;
}

/* -----------------------------------------------------------------------
 * Hex Dump Helper
 * ----------------------------------------------------------------------- */
static void hex_dump(const char *label, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    printf("%s (%u bytes):\n  ", label, len);
    for (i = 0; i < len; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0 && (i + 1) < len)
            printf("\n  ");
    }
    printf("\n");
}

/* -----------------------------------------------------------------------
 * Demo 1: Write-Only Transfer
 * ----------------------------------------------------------------------- */
static int demo_write(uint8_t cs)
{
    uint8_t tx_data[] = {
        0xAA, 0xBB, 0xCC, 0xDD,
        0x01, 0x02, 0x03, 0x04,
        0xDE, 0xAD, 0xBE, 0xEF
    };
    int ret;

    printf("\n=== Demo: Write-Only Transfer (CS%d) ===\n", cs);
    hex_dump("TX data", tx_data, sizeof(tx_data));

    ret = pru_spi_write(cs, tx_data, sizeof(tx_data), 0);
    if (ret < 0) {
        fprintf(stderr, "Write failed: %s\n", pru_spi_strerror(ret));
        return ret;
    }

    printf("Wrote %d bytes successfully\n", ret);
    return 0;
}

/* -----------------------------------------------------------------------
 * Demo 2: Full-Duplex Transfer
 * ----------------------------------------------------------------------- */
static int demo_full_duplex(uint8_t cs)
{
    uint8_t tx_data[] = {
        0x9F, 0x00, 0x00, 0x00  /* Example: JEDEC read ID command */
    };
    uint8_t rx_data[4] = {0};
    int ret;

    printf("\n=== Demo: Full-Duplex Transfer (CS%d) ===\n", cs);
    hex_dump("TX data", tx_data, sizeof(tx_data));

    ret = pru_spi_transfer(cs, tx_data, rx_data, sizeof(tx_data), 0);
    if (ret < 0) {
        fprintf(stderr, "Transfer failed: %s\n", pru_spi_strerror(ret));
        return ret;
    }

    hex_dump("RX data", rx_data, sizeof(rx_data));
    printf("Transferred %d bytes successfully\n", ret);
    return 0;
}

/* -----------------------------------------------------------------------
 * Demo 3: Read-Only Transfer
 * ----------------------------------------------------------------------- */
static int demo_read(uint8_t cs)
{
    uint8_t rx_data[8] = {0};
    int ret;

    printf("\n=== Demo: Read-Only Transfer (CS%d) ===\n", cs);
    printf("Sending 0x00 while reading %zu bytes...\n", sizeof(rx_data));

    ret = pru_spi_read(cs, rx_data, sizeof(rx_data), 0);
    if (ret < 0) {
        fprintf(stderr, "Read failed: %s\n", pru_spi_strerror(ret));
        return ret;
    }

    hex_dump("RX data", rx_data, sizeof(rx_data));
    printf("Read %d bytes successfully\n", ret);
    return 0;
}

/* -----------------------------------------------------------------------
 * Demo 4: Loopback Test (MOSI → MISO wired together)
 * ----------------------------------------------------------------------- */
static int demo_loopback(uint8_t cs)
{
    uint32_t test_sizes[] = {1, 4, 16, 64, 256, 1024};
    int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);
    int i, j;
    uint8_t *tx_buf, *rx_buf;
    int ret;
    int total_pass = 0, total_fail = 0;

    printf("\n=== Demo: Loopback Test (CS%d) ===\n", cs);
    printf("NOTE: Connect MOSI (P9_29) to MISO (P9_30) for this test\n\n");

    for (i = 0; i < num_tests; i++) {
        uint32_t size = test_sizes[i];
        int pass = 1;

        tx_buf = (uint8_t *)malloc(size);
        rx_buf = (uint8_t *)calloc(1, size);
        if (!tx_buf || !rx_buf) {
            fprintf(stderr, "Memory allocation failed\n");
            free(tx_buf);
            free(rx_buf);
            return -1;
        }

        /* Fill TX buffer with a pattern */
        for (j = 0; (uint32_t)j < size; j++)
            tx_buf[j] = (uint8_t)(j & 0xFF);

        /* Transfer */
        ret = pru_spi_transfer(cs, tx_buf, rx_buf, size, 0);
        if (ret < 0) {
            fprintf(stderr, "  [FAIL] %4u bytes: transfer error: %s\n",
                    size, pru_spi_strerror(ret));
            total_fail++;
            free(tx_buf);
            free(rx_buf);
            continue;
        }

        /* Compare */
        for (j = 0; (uint32_t)j < size; j++) {
            if (tx_buf[j] != rx_buf[j]) {
                pass = 0;
                fprintf(stderr, "  [FAIL] %4u bytes: mismatch at byte %d "
                        "(TX=0x%02X, RX=0x%02X)\n",
                        size, j, tx_buf[j], rx_buf[j]);
                break;
            }
        }

        if (pass) {
            printf("  [PASS] %4u bytes: data matches\n", size);
            total_pass++;
        } else {
            total_fail++;
        }

        free(tx_buf);
        free(rx_buf);
    }

    printf("\nLoopback results: %d passed, %d failed out of %d tests\n",
           total_pass, total_fail, num_tests);

    return (total_fail == 0) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Demo 5: Multi-CS Sequential Transfers
 * ----------------------------------------------------------------------- */
static int demo_multi_cs(void)
{
    uint8_t data[4];
    int cs, ret;

    printf("\n=== Demo: Multi-CS Sequential Transfers ===\n");

    for (cs = 0; cs < 4; cs++) {
        /* Different data for each CS line */
        data[0] = 0x10 + cs;
        data[1] = 0x20 + cs;
        data[2] = 0x30 + cs;
        data[3] = 0x40 + cs;

        printf("  CS%d: Writing [%02X %02X %02X %02X]... ",
               cs, data[0], data[1], data[2], data[3]);

        ret = pru_spi_write(cs, data, sizeof(data), 0);
        if (ret < 0) {
            fprintf(stderr, "FAILED: %s\n", pru_spi_strerror(ret));
            return ret;
        }

        printf("OK (%d bytes)\n", ret);
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * Demo 6: Large Transfer
 * ----------------------------------------------------------------------- */
static int demo_large_transfer(uint8_t cs, uint32_t size)
{
    uint8_t *tx_buf, *rx_buf;
    uint32_t i;
    int ret;

    printf("\n=== Demo: Large Transfer (%u bytes on CS%d) ===\n", size, cs);

    tx_buf = (uint8_t *)malloc(size);
    rx_buf = (uint8_t *)calloc(1, size);
    if (!tx_buf || !rx_buf) {
        fprintf(stderr, "Memory allocation failed for %u bytes\n", size);
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    /* Fill with incrementing pattern */
    for (i = 0; i < size; i++)
        tx_buf[i] = (uint8_t)(i & 0xFF);

    printf("Transferring %u bytes...\n", size);

    ret = pru_spi_transfer(cs, tx_buf, rx_buf, size, 10000);
    if (ret < 0) {
        fprintf(stderr, "Transfer failed: %s\n", pru_spi_strerror(ret));
        free(tx_buf);
        free(rx_buf);
        return ret;
    }

    printf("Transferred %d bytes successfully\n", ret);

    /* Show first and last 16 bytes */
    hex_dump("TX (first 16)", tx_buf, 16);
    hex_dump("RX (first 16)", rx_buf, 16);
    if (size > 32) {
        hex_dump("TX (last 16)", tx_buf + size - 16, 16);
        hex_dump("RX (last 16)", rx_buf + size - 16, 16);
    }

    free(tx_buf);
    free(rx_buf);
    return 0;
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int ret;
    int do_loopback = 0;
    int do_write_only = 0;
    uint32_t speed_hz = 0;
    uint8_t mode = 0xFF;  /* 0xFF = not set */
    int i;

    /* Parse command line */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loopback") == 0) {
            do_loopback = 1;
        } else if (strcmp(argv[i], "--write") == 0) {
            do_write_only = 1;
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            speed_hz = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --loopback   Run loopback test (wire MOSI to MISO)\n");
            printf("  --write      Run write-only demo\n");
            printf("  --speed N    Set SPI clock to N Hz (default: 10000000)\n");
            printf("  --mode M     Set SPI mode 0-3 (default: 0)\n");
            printf("  --help       Show this help\n");
            return 0;
        }
    }

    /* Install signal handler for clean shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("========================================\n");
    printf(" PRU SPI Bit-Bang — Example Application\n");
    printf("========================================\n");

    /* --- Initialize --- */
    ret = pru_spi_init();
    if (ret != PRU_SPI_OK) {
        fprintf(stderr, "FATAL: %s (error %d)\n", pru_spi_strerror(ret), ret);
        return 1;
    }

    /* --- Configure --- */
    if (mode != 0xFF) {
        ret = pru_spi_set_mode(mode);
        if (ret != PRU_SPI_OK) {
            fprintf(stderr, "Failed to set mode: %s\n", pru_spi_strerror(ret));
        }
    }

    if (speed_hz > 0) {
        uint32_t actual = pru_spi_set_speed(speed_hz);
        printf("Requested %u Hz, actual %u Hz\n", speed_hz, actual);
    }

    printf("\nConfiguration:\n");
    printf("  SPI Mode:  %d\n", pru_spi_get_mode());
    printf("  SPI Speed: %u Hz\n", pru_spi_get_speed());
    printf("  Max xfer:  %u bytes\n", pru_spi_get_max_transfer_size());
    printf("  Ready:     %s\n", pru_spi_is_ready() ? "yes" : "no");

    /* --- Run demos --- */

    if (do_loopback) {
        /* Loopback test only */
        ret = demo_loopback(0);
        if (ret == 0)
            printf("\n*** LOOPBACK TEST PASSED ***\n");
        else
            printf("\n*** LOOPBACK TEST FAILED ***\n");

    } else if (do_write_only) {
        /* Write-only demo */
        demo_write(0);

    } else {
        /* Run all demos */
        if (g_running) demo_write(0);
        if (g_running) demo_full_duplex(0);
        if (g_running) demo_read(0);
        if (g_running) demo_multi_cs();
        if (g_running) demo_large_transfer(0, 4096);
    }

    /* --- Cleanup --- */
    pru_spi_close();

    printf("\nDone.\n");
    return 0;
}
