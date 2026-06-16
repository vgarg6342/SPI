/*
 * pru_spi.c — ARM-side Implementation for PRU Bit-Banged SPI Master
 *
 * This module implements the pru_spi.h API by:
 *   - Managing PRU lifecycle via the remoteproc sysfs interface
 *   - Memory-mapping PRUSS registers and shared RAM via /dev/mem
 *   - Allocating DDR buffers for TX/RX data transfer
 *   - Communicating with the PRU firmware via a shared command block
 *
 * Memory Layout:
 *   PRUSS base (0x4A300000):
 *     +0x00000: PRU0 local data RAM (8KB) — staging buffers
 *     +0x02000: PRU1 local data RAM (8KB)
 *     +0x10000: Shared RAM (12KB) — command block at offset 0
 *
 *   DDR buffer: Allocated from a high physical address region
 *               accessible by both ARM and PRU via /dev/mem.
 *
 * Copyright (c) 2026 — MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "pru_spi.h"
#include "pru_spi_common.h"

/* -----------------------------------------------------------------------
 * Internal State
 * ----------------------------------------------------------------------- */

/* State flags */
static int g_initialized = 0;

/* /dev/mem file descriptor */
static int g_mem_fd = -1;

/* PRUSS mapped memory */
static void *g_pruss_map = NULL;

/* Pointer to command block in shared RAM */
static volatile struct pru_spi_cmd *g_cmd = NULL;

/* Streaming: control block + ring data, both in PRU shared RAM (no DDR) */
static volatile struct pru_dac_stream *g_strm = NULL;
static volatile struct pru_dac_frame  *g_ring = NULL;
static int g_streaming = 0;

/* DDR buffer physical and virtual addresses */
static uint32_t g_ddr_phys_addr = 0;
static void *g_ddr_map = NULL;
static uint32_t g_ddr_buf_size = 0;

/* Current SPI settings. NOTE: the SPI clock is fixed at COMPILE TIME via
 * SPI_SCLK_HZ in pru_spi_common.h (the PRU __delay_cycles() intrinsic needs a
 * constant), so there is no runtime clock divider. */
static uint8_t  g_spi_mode = SPI_MODE_0;
static const uint32_t g_spi_speed_hz = SPI_SCLK_HZ;

/* Parallel mode: how many MOSI lanes / DACs are active (1..NUM_DAC_LANES).
 * Lanes beyond this are held LOW (idle). Default: all 4. */
static uint8_t  g_num_dacs = NUM_DAC_LANES;

/*
 * DDR buffer base address for PRU access.
 *
 * We pick a physical address in the upper DDR region that is unlikely
 * to conflict with Linux kernel memory. On BeagleBone Black with 512MB
 * RAM (0x80000000 - 0x9FFFFFFF), we use 0x9F000000 (last 16MB).
 *
 * IMPORTANT: To guarantee this region is free, you should reserve it
 * in the device tree or bootloader. For development/testing, this
 * address range typically works if the system doesn't use all RAM.
 *
 * Alternative: Use the PRU shared RAM (12KB) for small transfers
 * (handled automatically when transfer_len <= available space).
 */
#define DDR_BUF_PHYS_BASE       0x9F000000
#define DDR_BUF_MAP_SIZE        (DDR_BUF_DEFAULT_SIZE * 2)  /* TX + RX */

/* TX buffer: first half of DDR region */
#define DDR_TX_OFFSET           0
/* RX buffer: second half of DDR region */
#define DDR_RX_OFFSET           DDR_BUF_DEFAULT_SIZE

/* -----------------------------------------------------------------------
 * Remoteproc Helpers
 * ----------------------------------------------------------------------- */

/**
 * Write a string to a sysfs file.
 * @return 0 on success, -1 on failure
 */
static int sysfs_write(const char *path, const char *value)
{
    int fd;
    ssize_t ret;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "pru_spi: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    ret = write(fd, value, strlen(value));
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "pru_spi: cannot write '%s' to %s: %s\n",
                value, path, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * Read a string from a sysfs file.
 * @return number of bytes read, or -1 on failure
 */
static int sysfs_read(const char *path, char *buf, size_t buf_size)
{
    int fd;
    ssize_t ret;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    ret = read(fd, buf, buf_size - 1);
    close(fd);

    if (ret < 0)
        return -1;

    buf[ret] = '\0';

    /* Strip trailing newline */
    if (ret > 0 && buf[ret - 1] == '\n')
        buf[ret - 1] = '\0';

    return (int)ret;
}

/**
 * Stop the PRU via remoteproc.
 */
static int pru_stop(void)
{
    char state[32];
    char path[256];

    snprintf(path, sizeof(path), "%s/state", REMOTEPROC_PRU0_PATH);

    /* Check current state */
    if (sysfs_read(path, state, sizeof(state)) >= 0) {
        if (strcmp(state, "running") == 0) {
            return sysfs_write(path, "stop");
        }
    }

    return 0;   /* Already stopped or doesn't exist */
}

/**
 * Load and start the PRU firmware.
 */
static int pru_start(void)
{
    char fw_path[256];
    char state_path[256];

    snprintf(fw_path, sizeof(fw_path), "%s/firmware", REMOTEPROC_PRU0_PATH);
    snprintf(state_path, sizeof(state_path), "%s/state", REMOTEPROC_PRU0_PATH);

    /* Stop PRU first if running */
    pru_stop();
    usleep(100000);  /* 100ms settling time */

    /* Set firmware name */
    if (sysfs_write(fw_path, PRU_FW_NAME) < 0) {
        fprintf(stderr, "pru_spi: failed to set firmware name\n");
        return PRU_SPI_ERR_FIRMWARE;
    }

    /* Start PRU */
    if (sysfs_write(state_path, "start") < 0) {
        fprintf(stderr, "pru_spi: failed to start PRU\n");
        return PRU_SPI_ERR_FIRMWARE;
    }

    return PRU_SPI_OK;
}

/* -----------------------------------------------------------------------
 * Memory Mapping
 * ----------------------------------------------------------------------- */

/**
 * Map PRUSS memory region via /dev/mem.
 */
static int map_pruss_memory(void)
{
    g_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_mem_fd < 0) {
        fprintf(stderr, "pru_spi: cannot open /dev/mem: %s\n"
                "  (are you running as root?)\n", strerror(errno));
        return PRU_SPI_ERR_MMAP;
    }

    /* Map PRUSS region */
    g_pruss_map = mmap(NULL, PRUSS_MAP_SIZE,
                       PROT_READ | PROT_WRITE, MAP_SHARED,
                       g_mem_fd, PRUSS_BASE_ADDR);
    if (g_pruss_map == MAP_FAILED) {
        fprintf(stderr, "pru_spi: mmap PRUSS failed: %s\n", strerror(errno));
        g_pruss_map = NULL;
        return PRU_SPI_ERR_MMAP;
    }

    /* Command block, stream control block, and ring data all live in shared RAM */
    g_cmd = (volatile struct pru_spi_cmd *)
        ((uint8_t *)g_pruss_map + PRU_SHAREDMEM_OFFSET + CMD_BLOCK_SHMEM_OFFSET);
    g_strm = (volatile struct pru_dac_stream *)
        ((uint8_t *)g_pruss_map + PRU_SHAREDMEM_OFFSET + STREAM_CTRL_SHMEM_OFFSET);
    g_ring = (volatile struct pru_dac_frame *)
        ((uint8_t *)g_pruss_map + PRU_SHAREDMEM_OFFSET + RING_DATA_SHMEM_OFFSET);

    return PRU_SPI_OK;
}

/**
 * Map DDR buffer region for TX/RX data.
 */
static int map_ddr_buffer(void)
{
    g_ddr_buf_size = DDR_BUF_DEFAULT_SIZE;
    g_ddr_phys_addr = DDR_BUF_PHYS_BASE;

    g_ddr_map = mmap(NULL, DDR_BUF_MAP_SIZE,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     g_mem_fd, g_ddr_phys_addr);
    if (g_ddr_map == MAP_FAILED) {
        fprintf(stderr, "pru_spi: mmap DDR buffer at 0x%08X failed: %s\n",
                g_ddr_phys_addr, strerror(errno));
        g_ddr_map = NULL;
        return PRU_SPI_ERR_MMAP;
    }

    /* Zero out the buffers */
    memset(g_ddr_map, 0, DDR_BUF_MAP_SIZE);

    return PRU_SPI_OK;
}

/**
 * Unmap all memory regions.
 */
static void unmap_memory(void)
{
    if (g_ddr_map && g_ddr_map != MAP_FAILED) {
        munmap(g_ddr_map, DDR_BUF_MAP_SIZE);
        g_ddr_map = NULL;
    }

    if (g_pruss_map && g_pruss_map != MAP_FAILED) {
        munmap(g_pruss_map, PRUSS_MAP_SIZE);
        g_pruss_map = NULL;
    }

    g_cmd = NULL;

    if (g_mem_fd >= 0) {
        close(g_mem_fd);
        g_mem_fd = -1;
    }
}

/* -----------------------------------------------------------------------
 * Timing Helper
 * ----------------------------------------------------------------------- */

/**
 * Get current time in milliseconds (monotonic clock).
 */
static uint64_t time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* -----------------------------------------------------------------------
 * Public API Implementation
 * ----------------------------------------------------------------------- */

int pru_spi_init(void)
{
    int ret;
    uint64_t start;

    if (g_initialized) {
        fprintf(stderr, "pru_spi: already initialized\n");
        return PRU_SPI_OK;
    }

    printf("pru_spi: initializing PRU SPI subsystem...\n");

    /* Step 1: Map PRUSS memory */
    ret = map_pruss_memory();
    if (ret != PRU_SPI_OK) {
        unmap_memory();
        return ret;
    }
    printf("pru_spi: PRUSS memory mapped at %p\n", g_pruss_map);

    /* Step 2: Map DDR buffers */
    ret = map_ddr_buffer();
    if (ret != PRU_SPI_OK) {
        unmap_memory();
        return ret;
    }
    printf("pru_spi: DDR buffer mapped at %p (phys 0x%08X, size %u KB)\n",
           g_ddr_map, g_ddr_phys_addr, DDR_BUF_MAP_SIZE / 1024);

    /* Step 3: Load and start PRU firmware */
    ret = pru_start();
    if (ret != PRU_SPI_OK) {
        unmap_memory();
        return ret;
    }
    printf("pru_spi: PRU firmware loaded and started\n");

    /* Step 4: Wait for PRU to signal readiness (magic number set) */
    start = time_ms();
    while (g_cmd->magic != PRU_SPI_MAGIC) {
        if (time_ms() - start > 3000) {
            fprintf(stderr, "pru_spi: timeout waiting for PRU readiness\n");
            fprintf(stderr, "  magic = 0x%08X (expected 0x%08X)\n",
                    g_cmd->magic, PRU_SPI_MAGIC);
            pru_stop();
            unmap_memory();
            return PRU_SPI_ERR_FIRMWARE;
        }
        usleep(1000);  /* 1ms poll interval */
    }

    /* Step 5: Set default configuration. SPI clock is compile-time fixed. */
    g_spi_mode = SPI_MODE_0;

    g_initialized = 1;
    printf("pru_spi: initialized successfully (SPI mode %d, %u Hz fixed)\n",
           g_spi_mode, g_spi_speed_hz);

    return PRU_SPI_OK;
}

void pru_spi_close(void)
{
    if (!g_initialized)
        return;

    printf("pru_spi: shutting down...\n");

    /* Send shutdown command to PRU */
    if (g_cmd) {
        g_cmd->command = CMD_SHUTDOWN;

        /* Brief wait for PRU to acknowledge */
        usleep(10000);  /* 10ms */
    }

    /* Stop PRU via remoteproc */
    pru_stop();

    /* Unmap memory */
    unmap_memory();

    g_initialized = 0;
    printf("pru_spi: shutdown complete\n");
}

int pru_spi_transfer(uint8_t cs, const uint8_t *tx_buf,
                     uint8_t *rx_buf, uint32_t len,
                     uint32_t timeout_ms)
{
    uint8_t *ddr_tx;
    uint8_t *ddr_rx;
    uint64_t start;
    uint32_t status;

    /* --- Validation --- */

    if (!g_initialized)
        return PRU_SPI_ERR_NOT_INIT;

    if (cs >= PRU_SPI_MAX_CS)
        return PRU_SPI_ERR_PARAM;

    if (len == 0)
        return PRU_SPI_ERR_PARAM;

    if (len > g_ddr_buf_size) {
        fprintf(stderr, "pru_spi: transfer size %u exceeds buffer %u\n",
                len, g_ddr_buf_size);
        return PRU_SPI_ERR_PARAM;
    }

    if (tx_buf == NULL && rx_buf == NULL)
        return PRU_SPI_ERR_PARAM;

    /* Use default timeout if 0 */
    if (timeout_ms == 0)
        timeout_ms = PRU_SPI_DEFAULT_TIMEOUT_MS;

    /* --- Check PRU is idle --- */

    if (g_cmd->status == STATUS_BUSY)
        return PRU_SPI_ERR_BUSY;

    /* --- Prepare DDR buffers --- */

    ddr_tx = (uint8_t *)g_ddr_map + DDR_TX_OFFSET;
    ddr_rx = (uint8_t *)g_ddr_map + DDR_RX_OFFSET;

    /* Copy TX data to DDR buffer */
    if (tx_buf != NULL) {
        memcpy(ddr_tx, tx_buf, len);
    } else {
        /* Read-only transfer: fill TX buffer with 0x00 */
        memset(ddr_tx, 0x00, len);
    }

    /* Clear RX buffer */
    memset(ddr_rx, 0x00, len);

    /* --- Issue command to PRU --- */

    g_cmd->cs_line      = cs;
    g_cmd->spi_mode     = g_spi_mode;
    g_cmd->tx_ddr_addr  = g_ddr_phys_addr + DDR_TX_OFFSET;
    g_cmd->rx_ddr_addr  = g_ddr_phys_addr + DDR_RX_OFFSET;
    g_cmd->transfer_len = len;
    g_cmd->status       = STATUS_IDLE;
    g_cmd->bytes_done   = 0;
    g_cmd->error_code   = ERR_NONE;

    /*
     * Memory barrier: ensure all writes above are visible to the PRU
     * before we set the command field (which triggers execution).
     */
    __sync_synchronize();

    /* Trigger the transfer */
    g_cmd->command = CMD_TRANSFER;

    /* --- Wait for completion --- */

    start = time_ms();
    do {
        status = g_cmd->status;

        if (status == STATUS_DONE)
            break;

        if (status == STATUS_ERROR) {
            fprintf(stderr, "pru_spi: transfer error (code %u)\n",
                    g_cmd->error_code);
            return PRU_SPI_ERR_PRU;
        }

        if (time_ms() - start > timeout_ms) {
            fprintf(stderr, "pru_spi: transfer timeout (%u ms, %u/%u bytes done)\n",
                    timeout_ms, g_cmd->bytes_done, len);
            return PRU_SPI_ERR_TIMEOUT;
        }

        /*
         * Brief sleep between polls to avoid hammering the bus.
         * For small transfers at 10MHz, completion time is:
         *   100 bytes  ~100µs
         *   1KB        ~1ms
         *   64KB       ~65ms
         *
         * We use adaptive sleep: short for small transfers, longer for big ones.
         */
        if (len < 256)
            usleep(10);     /* 10µs for small transfers */
        else if (len < 4096)
            usleep(100);    /* 100µs for medium */
        else
            usleep(1000);   /* 1ms for large */

    } while (1);

    /* --- Copy RX data back to user buffer --- */

    if (rx_buf != NULL) {
        memcpy(rx_buf, ddr_rx, len);
    }

    return (int)len;
}

int pru_spi_write(uint8_t cs, const uint8_t *tx_buf,
                  uint32_t len, uint32_t timeout_ms)
{
    if (tx_buf == NULL)
        return PRU_SPI_ERR_PARAM;

    return pru_spi_transfer(cs, tx_buf, NULL, len, timeout_ms);
}

int pru_spi_read(uint8_t cs, uint8_t *rx_buf,
                 uint32_t len, uint32_t timeout_ms)
{
    if (rx_buf == NULL)
        return PRU_SPI_ERR_PARAM;

    return pru_spi_transfer(cs, NULL, rx_buf, len, timeout_ms);
}

/* -----------------------------------------------------------------------
 * Parallel (4-lane) Write — drive 4 DACs simultaneously
 *
 * Each DAC gets its OWN data on its OWN MOSI lane; SCLK and the CS/SYNC line
 * are shared. We pre-transpose the four word streams into one stream of
 * "R30-ready" words (one word per bit-time, MOSI lane bits placed at
 * PAR_MOSI0/1/2/3, SCLK and CS bits 0) so the PRU can blast it out with a
 * single store per bit and update all four lanes on every shared clock edge.
 * The PRU drives the shared CS (active LOW) to frame and latch each word —
 * all 4 DACs latch together; no ARM GPIO is needed.
 * ----------------------------------------------------------------------- */
int pru_spi_parallel_write(const uint16_t *dac0, const uint16_t *dac1,
                           const uint16_t *dac2, const uint16_t *dac3,
                           uint32_t num_frames, uint32_t timeout_ms)
{
    static const uint32_t lane_bit[NUM_DAC_LANES] = PAR_MOSI_LOOKUP;
    const uint16_t *dac[NUM_DAC_LANES];
    uint32_t *stream;
    uint32_t frame_bits = SPI_FRAME_BITS;
    uint32_t total_words;
    uint32_t f, k, d, bitpos;
    uint64_t start;
    uint32_t status;

    if (!g_initialized)
        return PRU_SPI_ERR_NOT_INIT;

    if (num_frames == 0)
        return PRU_SPI_ERR_PARAM;

    dac[0] = dac0; dac[1] = dac1; dac[2] = dac2; dac[3] = dac3;

    total_words = num_frames * frame_bits;

    /* Stream lives in the DDR TX buffer as 32-bit R30 words */
    if (total_words * sizeof(uint32_t) > g_ddr_buf_size) {
        fprintf(stderr, "pru_spi: parallel stream (%u words) exceeds buffer %u\n",
                total_words, g_ddr_buf_size);
        return PRU_SPI_ERR_PARAM;
    }

    if (g_cmd->status == STATUS_BUSY)
        return PRU_SPI_ERR_BUSY;

    if (timeout_ms == 0)
        timeout_ms = PRU_SPI_DEFAULT_TIMEOUT_MS;

    /* --- Bit-transpose into the R30-ready stream (MSB first) --- */
    stream = (uint32_t *)((uint8_t *)g_ddr_map + DDR_TX_OFFSET);

    for (f = 0; f < num_frames; f++) {
        for (k = 0; k < frame_bits; k++) {
            uint32_t word = 0;
            bitpos = frame_bits - 1 - k;        /* k==0 -> MSB */
            for (d = 0; d < g_num_dacs; d++) {  /* lanes >= g_num_dacs stay LOW */
                if (dac[d] != NULL && ((dac[d][f] >> bitpos) & 1U))
                    word |= lane_bit[d];
            }
            stream[f * frame_bits + k] = word;
        }
    }

    /* --- Issue the parallel command --- */
    g_cmd->spi_mode     = g_spi_mode;
    g_cmd->tx_ddr_addr  = g_ddr_phys_addr + DDR_TX_OFFSET;
    g_cmd->num_frames   = num_frames;
    g_cmd->frame_bits   = frame_bits;
    g_cmd->num_lanes    = g_num_dacs;
    g_cmd->transfer_len = 0;
    g_cmd->status       = STATUS_IDLE;
    g_cmd->bytes_done   = 0;
    g_cmd->error_code   = ERR_NONE;

    __sync_synchronize();

    g_cmd->command = CMD_TRANSFER_PARALLEL;

    /* --- Wait for completion --- */
    start = time_ms();
    do {
        status = g_cmd->status;

        if (status == STATUS_DONE)
            break;

        if (status == STATUS_ERROR) {
            fprintf(stderr, "pru_spi: parallel transfer error (code %u)\n",
                    g_cmd->error_code);
            return PRU_SPI_ERR_PRU;
        }

        if (time_ms() - start > timeout_ms) {
            fprintf(stderr, "pru_spi: parallel transfer timeout (%u ms)\n",
                    timeout_ms);
            return PRU_SPI_ERR_TIMEOUT;
        }

        usleep(num_frames < 64 ? 10 : 100);
    } while (1);

    return (int)num_frames;
}

/* -----------------------------------------------------------------------
 * Continuous 4-DAC Streaming (shared-RAM ring buffer, no DDR)
 *
 * Lock-free single-producer (this ARM code owns 'head') / single-consumer (PRU
 * owns 'tail'). The PRU paces consumption off its IEP timer, so the ARM side
 * just has to keep the ring fed. With a 1024-frame ring (~102 ms at 10 kHz),
 * ordinary scheduler jitter cannot starve the consumer.
 * ----------------------------------------------------------------------- */

int pru_dac_stream_start(uint32_t sample_rate_hz)
{
    uint32_t period;

    if (!g_initialized)
        return PRU_SPI_ERR_NOT_INIT;

    if (g_streaming)
        return PRU_SPI_ERR_BUSY;

    if (g_cmd->status == STATUS_BUSY || g_cmd->status == STATUS_STREAMING)
        return PRU_SPI_ERR_BUSY;

    /* Compute the PRU IEP period in cycles. 0 => firmware default (10 kHz). */
    if (sample_rate_hz == 0) {
        period = DAC_SAMPLE_PERIOD_CYCLES;
    } else {
        period = PRU_CLK_HZ / sample_rate_hz;
        if (period == 0)
            return PRU_SPI_ERR_PARAM;   /* rate too high */
    }

    /* Reset the ring and program the control block while the PRU is idle. */
    g_strm->head = 0;
    g_strm->tail = 0;
    g_strm->capacity = RING_CAPACITY_FRAMES;
    g_strm->sample_period_cycles = period;
    g_strm->eos = 0;
    g_strm->underflow_count = 0;
    g_strm->frames_played = 0;

    g_cmd->spi_mode   = SPI_MODE_0;
    g_cmd->status     = STATUS_IDLE;
    g_cmd->error_code = ERR_NONE;

    __sync_synchronize();

    g_cmd->command = CMD_STREAM_START;

    g_streaming = 1;
    return PRU_SPI_OK;
}

int pru_dac_stream_push(const uint16_t dac_values[4])
{
    uint32_t head, tail, idx;
    uint64_t start;

    if (!g_streaming)
        return PRU_SPI_ERR_NOT_INIT;
    if (dac_values == NULL)
        return PRU_SPI_ERR_PARAM;

    head = g_strm->head;

    /* Backpressure: wait while the ring is full. The PRU drains at the sample
     * rate, so a free slot appears within one sample period. */
    start = time_ms();
    for (;;) {
        tail = g_strm->tail;
        if ((uint32_t)(head - tail) < RING_CAPACITY_FRAMES)
            break;

        if (g_cmd->status == STATUS_ERROR)
            return PRU_SPI_ERR_PRU;
        if (g_cmd->status == STATUS_DONE)
            return PRU_SPI_ERR_PRU;     /* PRU stopped unexpectedly */
        if (time_ms() - start > PRU_SPI_DEFAULT_TIMEOUT_MS)
            return PRU_SPI_ERR_TIMEOUT;

        usleep(50);
    }

    /* Write the framed words into the open slot, then publish via head++. */
    idx = head & RING_INDEX_MASK;
    g_ring[idx].w[0] = DAC_FRAME(dac_values[0]);
    g_ring[idx].w[1] = DAC_FRAME(dac_values[1]);
    g_ring[idx].w[2] = DAC_FRAME(dac_values[2]);
    g_ring[idx].w[3] = DAC_FRAME(dac_values[3]);

    __sync_synchronize();               /* data visible before head advances */
    g_strm->head = head + 1;

    return PRU_SPI_OK;
}

int pru_dac_stream_end(uint32_t timeout_ms)
{
    uint64_t start;
    int underflows;

    if (!g_streaming)
        return PRU_SPI_ERR_NOT_INIT;

    if (timeout_ms == 0)
        timeout_ms = PRU_SPI_DEFAULT_TIMEOUT_MS;

    /* Signal end-of-stream; the PRU drains remaining frames then sets DONE. */
    __sync_synchronize();
    g_strm->eos = 1;

    /* Wait until the PRU finishes. Status progresses IDLE -> STREAMING -> DONE,
     * so wait for DONE rather than "while STREAMING" (avoids a small-file race
     * where the PRU hasn't entered the loop yet). */
    start = time_ms();
    while (g_cmd->status != STATUS_DONE) {
        if (g_cmd->status == STATUS_ERROR)
            break;                          /* drained; underflow is advisory */
        if (time_ms() - start > timeout_ms) {
            /* Force a stop if the PRU is wedged. */
            g_cmd->command = CMD_STREAM_STOP;
            g_streaming = 0;
            return PRU_SPI_ERR_TIMEOUT;
        }
        usleep(200);
    }

    underflows = (int)g_strm->underflow_count;
    g_streaming = 0;
    return underflows;
}

int pru_spi_parallel_write_one(uint16_t d0, uint16_t d1,
                               uint16_t d2, uint16_t d3,
                               uint32_t timeout_ms)
{
    uint16_t v0 = d0, v1 = d1, v2 = d2, v3 = d3;
    return pru_spi_parallel_write(&v0, &v1, &v2, &v3, 1, timeout_ms);
}

int pru_spi_set_num_dacs(uint8_t n)
{
    if (n < 1 || n > NUM_DAC_LANES)
        return PRU_SPI_ERR_PARAM;

    g_num_dacs = n;
    printf("pru_spi: parallel active DACs/lanes set to %u "
           "(MOSI0..MOSI%u; higher lanes idle LOW)\n", n, (unsigned)(n - 1));
    return PRU_SPI_OK;
}

int pru_spi_get_num_dacs(void)
{
    if (!g_initialized)
        return PRU_SPI_ERR_NOT_INIT;
    return (int)g_num_dacs;
}

int pru_spi_set_mode(uint8_t mode)
{
    /* This firmware supports SPI mode 0 only (CPOL=0, CPHA=0). */
    if (mode != SPI_MODE_0) {
        fprintf(stderr,
                "pru_spi: only SPI mode 0 is supported (requested %u)\n", mode);
        return PRU_SPI_ERR_PARAM;
    }

    g_spi_mode = SPI_MODE_0;
    return PRU_SPI_OK;
}

uint32_t pru_spi_set_speed(uint32_t hz)
{
    /* The SPI clock is fixed at compile time via SPI_SCLK_HZ in
     * pru_spi_common.h (the PRU __delay_cycles() intrinsic requires a
     * compile-time constant). This call cannot change it at runtime. */
    fprintf(stderr,
            "pru_spi: speed is compile-time fixed at %u Hz "
            "(requested %u ignored; edit SPI_SCLK_HZ and rebuild)\n",
            g_spi_speed_hz, hz);

    return g_spi_speed_hz;
}

int pru_spi_get_mode(void)
{
    if (!g_initialized)
        return PRU_SPI_ERR_NOT_INIT;
    return (int)g_spi_mode;
}

uint32_t pru_spi_get_speed(void)
{
    if (!g_initialized)
        return 0;
    return g_spi_speed_hz;
}

uint32_t pru_spi_get_max_transfer_size(void)
{
    return DDR_BUF_DEFAULT_SIZE;
}

int pru_spi_is_ready(void)
{
    if (!g_initialized || !g_cmd)
        return 0;

    return (g_cmd->magic == PRU_SPI_MAGIC &&
            g_cmd->status != STATUS_BUSY);
}

const char *pru_spi_strerror(int err)
{
    switch (err) {
    case PRU_SPI_OK:           return "Success";
    case PRU_SPI_ERR_INIT:     return "Initialization failed";
    case PRU_SPI_ERR_MMAP:     return "Memory mapping failed (need root?)";
    case PRU_SPI_ERR_FIRMWARE: return "Firmware loading failed";
    case PRU_SPI_ERR_TIMEOUT:  return "Transfer timed out";
    case PRU_SPI_ERR_PARAM:    return "Invalid parameter";
    case PRU_SPI_ERR_NOT_INIT: return "Not initialized (call pru_spi_init first)";
    case PRU_SPI_ERR_BUSY:     return "PRU is busy";
    case PRU_SPI_ERR_PRU:      return "PRU reported error";
    case PRU_SPI_ERR_ALLOC:    return "Buffer allocation failed";
    default:                   return "Unknown error";
    }
}
