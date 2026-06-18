/*
 * pru_spi.c — ARM-side implementation of the PRU single-DAC player
 *
 * Responsibilities:
 *   - load/start/stop PRU0 via the remoteproc sysfs interface
 *   - mmap the PRUSS shared RAM (command block) and the DDR sample buffer
 *   - frame 12-bit codes into 16-bit words, drop them in DDR, kick the PRU
 *   - watch the PRU's heartbeat so a crashed/hung PRU is reported, not waited on
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

#include "pru_spi.h"
#include "pru_spi_common.h"

/* -----------------------------------------------------------------------
 * Module state
 * ----------------------------------------------------------------------- */
static int       g_initialized = 0;
static int       g_mem_fd      = -1;
static void     *g_pruss_map   = NULL;            /* PRUSS registers + RAM   */
static void     *g_ddr_map     = NULL;            /* DDR sample buffer       */
static volatile struct pru_dac_cmd *g_cmd = NULL; /* command block in shmem  */

/* Floor for the hung-PRU watchdog (ms). The PRU bumps the heartbeat once per
 * sample (and every idle poll); during a delay-paced sample it is blocked, so
 * the gap between bumps is up to one sample period. pru_dac_play() therefore
 * uses max(this floor, several sample periods) so a low sample rate does not
 * false-trigger. Only a real crash/hang freezes the heartbeat for longer. */
#define WATCHDOG_FLOOR_MS  300u

/* -----------------------------------------------------------------------
 * sysfs helpers (remoteproc lives under /sys)
 * ----------------------------------------------------------------------- */
static int sysfs_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    ssize_t ret;

    if (fd < 0) {
        fprintf(stderr, "pru_dac: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    ret = write(fd, value, strlen(value));
    close(fd);
    if (ret < 0) {
        fprintf(stderr, "pru_dac: cannot write '%s' to %s: %s\n",
                value, path, strerror(errno));
        return -1;
    }
    return 0;
}

static int sysfs_read(const char *path, char *buf, size_t buf_size)
{
    int fd = open(path, O_RDONLY);
    ssize_t ret;

    if (fd < 0)
        return -1;
    ret = read(fd, buf, buf_size - 1);
    close(fd);
    if (ret < 0)
        return -1;
    buf[ret] = '\0';
    if (ret > 0 && buf[ret - 1] == '\n')
        buf[ret - 1] = '\0';
    return (int)ret;
}

static int pru_stop(void)
{
    char state[32];
    char path[256];

    snprintf(path, sizeof(path), "%s/state", REMOTEPROC_PRU0_PATH);
    if (sysfs_read(path, state, sizeof(state)) >= 0 &&
        strcmp(state, "running") == 0)
        return sysfs_write(path, "stop");
    return 0;
}

static int pru_start(void)
{
    char fw_path[256], state_path[256];

    snprintf(fw_path, sizeof(fw_path), "%s/firmware", REMOTEPROC_PRU0_PATH);
    snprintf(state_path, sizeof(state_path), "%s/state", REMOTEPROC_PRU0_PATH);

    pru_stop();
    usleep(100000);   /* 100 ms settle */

    if (sysfs_write(fw_path, PRU_FW_NAME) < 0 ||
        sysfs_write(state_path, "start") < 0)
        return PRU_DAC_ERR_FIRMWARE;
    return PRU_DAC_OK;
}

/* -----------------------------------------------------------------------
 * Memory mapping
 * ----------------------------------------------------------------------- */
static int map_memory(void)
{
    g_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_mem_fd < 0) {
        fprintf(stderr, "pru_dac: cannot open /dev/mem: %s (run as root?)\n",
                strerror(errno));
        return PRU_DAC_ERR_MMAP;
    }

    g_pruss_map = mmap(NULL, PRUSS_MAP_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, g_mem_fd, PRUSS_BASE_ADDR);
    if (g_pruss_map == MAP_FAILED) {
        fprintf(stderr, "pru_dac: mmap PRUSS failed: %s\n", strerror(errno));
        g_pruss_map = NULL;
        return PRU_DAC_ERR_MMAP;
    }
    g_cmd = (volatile struct pru_dac_cmd *)
        ((uint8_t *)g_pruss_map + PRU_SHAREDMEM_OFFSET + CMD_BLOCK_SHMEM_OFFSET);

    g_ddr_map = mmap(NULL, DDR_BUF_BYTES, PROT_READ | PROT_WRITE,
                     MAP_SHARED, g_mem_fd, DDR_BUF_PHYS_BASE);
    if (g_ddr_map == MAP_FAILED) {
        fprintf(stderr,
                "pru_dac: mmap DDR buffer at 0x%08X (%u bytes) failed: %s\n"
                "  Is the region reserved? See USER_MANUAL.md §3 (uEnv.txt mem=).\n",
                DDR_BUF_PHYS_BASE, (unsigned)DDR_BUF_BYTES, strerror(errno));
        g_ddr_map = NULL;
        return PRU_DAC_ERR_MMAP;
    }
    memset(g_ddr_map, 0, DDR_BUF_BYTES);
    return PRU_DAC_OK;
}

static void unmap_memory(void)
{
    if (g_ddr_map && g_ddr_map != MAP_FAILED)
        munmap(g_ddr_map, DDR_BUF_BYTES);
    if (g_pruss_map && g_pruss_map != MAP_FAILED)
        munmap(g_pruss_map, PRUSS_MAP_SIZE);
    g_ddr_map = NULL;
    g_pruss_map = NULL;
    g_cmd = NULL;
    if (g_mem_fd >= 0) {
        close(g_mem_fd);
        g_mem_fd = -1;
    }
}

/* -----------------------------------------------------------------------
 * Timing helper
 * ----------------------------------------------------------------------- */
static uint64_t time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
int pru_dac_init(void)
{
    int ret;
    uint64_t start;

    if (g_initialized)
        return PRU_DAC_OK;

    printf("pru_dac: initializing...\n");

    ret = map_memory();
    if (ret != PRU_DAC_OK) {
        unmap_memory();
        return ret;
    }

    ret = pru_start();
    if (ret != PRU_DAC_OK) {
        unmap_memory();
        return ret;
    }

    /* Wait for the firmware to publish the magic. */
    start = time_ms();
    while (g_cmd->magic != PRU_DAC_MAGIC) {
        if (time_ms() - start > 3000) {
            fprintf(stderr, "pru_dac: PRU not ready (magic=0x%08X, want 0x%08X)\n",
                    g_cmd->magic, PRU_DAC_MAGIC);
            pru_stop();
            unmap_memory();
            return PRU_DAC_ERR_FIRMWARE;
        }
        usleep(1000);
    }

    g_initialized = 1;
    printf("pru_dac: ready (SPI mode 0, %u Hz bit clock, %u Hz default sample "
           "rate, DDR buffer %u samples)\n",
           (unsigned)SPI_SCLK_HZ, (unsigned)DAC_SAMPLE_RATE_HZ,
           (unsigned)STREAM_MAX_SAMPLES);
    return PRU_DAC_OK;
}

void pru_dac_close(void)
{
    if (!g_initialized)
        return;
    printf("pru_dac: shutting down...\n");
    if (g_cmd) {
        g_cmd->command = CMD_SHUTDOWN;
        usleep(10000);
    }
    pru_stop();
    unmap_memory();
    g_initialized = 0;
}

int pru_dac_play(const uint16_t *codes, uint32_t num_samples,
                 uint32_t timeout_ms)
{
    volatile uint16_t *ddr = (volatile uint16_t *)g_ddr_map;
    uint32_t i, last_hb;
    uint64_t start, last_progress;
    /* Allow several sample periods (5000/rate ms = 5 periods) above the floor,
     * so low sample rates don't look like a hang. */
    uint32_t watchdog_ms = WATCHDOG_FLOOR_MS + (5000u / DAC_SAMPLE_RATE_HZ);

    if (!g_initialized)
        return PRU_DAC_ERR_NOT_INIT;
    if (codes == NULL || num_samples == 0)
        return PRU_DAC_ERR_PARAM;
    if (num_samples > STREAM_MAX_SAMPLES) {
        fprintf(stderr, "pru_dac: %u samples exceeds DDR capacity %u "
                "(raise STREAM_DURATION_SEC and rebuild)\n",
                num_samples, (unsigned)STREAM_MAX_SAMPLES);
        return PRU_DAC_ERR_TOO_LARGE;
    }
    if (g_cmd->status == STATUS_PLAYING)
        return PRU_DAC_ERR_BUSY;

    /* Frame the codes into DDR (apply the 4 DAC control bits here). */
    for (i = 0; i < num_samples; i++)
        ddr[i] = DAC_FRAME(codes[i]);

    /* Default timeout: playback time (at the compile-time rate) + 1 s margin. */
    if (timeout_ms == 0)
        timeout_ms = (uint32_t)(((uint64_t)num_samples * 1000u) /
                                DAC_SAMPLE_RATE_HZ) + 1000u;

    g_cmd->tx_ddr_addr  = DDR_BUF_PHYS_BASE;
    g_cmd->num_samples  = num_samples;
    g_cmd->samples_done = 0;
    g_cmd->error_code   = ERR_NONE;
    g_cmd->status       = STATUS_IDLE;

    __sync_synchronize();               /* publish all fields before the command */
    last_hb = g_cmd->heartbeat;
    g_cmd->command = CMD_PLAY;

    /* Wait for completion, watching the heartbeat for a hung PRU. */
    start = last_progress = time_ms();
    for (;;) {
        uint32_t status = g_cmd->status;
        uint32_t hb     = g_cmd->heartbeat;
        uint64_t now    = time_ms();

        if (status == STATUS_DONE)
            return (int)num_samples;
        if (status == STATUS_ERROR) {
            fprintf(stderr, "pru_dac: PRU error (code %u)\n", g_cmd->error_code);
            return PRU_DAC_ERR_PRU;
        }

        if (hb != last_hb) {
            last_hb = hb;
            last_progress = now;
        } else if (now - last_progress > watchdog_ms) {
            char state[32] = "?";
            char path[256];
            snprintf(path, sizeof(path), "%s/state", REMOTEPROC_PRU0_PATH);
            sysfs_read(path, state, sizeof(state));
            fprintf(stderr,
                    "pru_dac: PRU HUNG — heartbeat frozen for %u ms "
                    "(remoteproc state '%s', %u/%u samples done).\n"
                    "  The firmware crashed mid-run. Check: dmesg | tail -20\n",
                    watchdog_ms, state, g_cmd->samples_done, num_samples);
            return PRU_DAC_ERR_HUNG;
        }

        if (now - start > timeout_ms) {
            g_cmd->command = CMD_STOP;
            fprintf(stderr, "pru_dac: timeout after %u ms (%u/%u samples)\n",
                    timeout_ms, g_cmd->samples_done, num_samples);
            return PRU_DAC_ERR_TIMEOUT;
        }

        usleep(2000);
    }
}

int pru_dac_is_alive(void)
{
    uint32_t hb0;

    if (!g_initialized)
        return PRU_DAC_ERR_NOT_INIT;

    hb0 = g_cmd->heartbeat;
    usleep(5000);                       /* idle loop bumps heartbeat very fast */
    return (g_cmd->heartbeat != hb0) ? 1 : 0;
}

int pru_dac_get_state(char *buf, size_t buflen)
{
    char path[256];

    if (buf == NULL || buflen == 0)
        return PRU_DAC_ERR_PARAM;
    snprintf(path, sizeof(path), "%s/state", REMOTEPROC_PRU0_PATH);
    if (sysfs_read(path, buf, buflen) < 0)
        return PRU_DAC_ERR_FIRMWARE;
    return PRU_DAC_OK;
}

uint32_t pru_dac_capacity_samples(void)
{
    return STREAM_MAX_SAMPLES;
}

const char *pru_dac_strerror(int err)
{
    switch (err) {
    case PRU_DAC_OK:            return "Success";
    case PRU_DAC_ERR_NOT_INIT:  return "Not initialized (call pru_dac_init first)";
    case PRU_DAC_ERR_MMAP:      return "Memory mapping failed (need root? DDR reserved?)";
    case PRU_DAC_ERR_FIRMWARE:  return "Firmware load/start failed";
    case PRU_DAC_ERR_PARAM:     return "Invalid parameter";
    case PRU_DAC_ERR_BUSY:      return "PRU is busy";
    case PRU_DAC_ERR_TIMEOUT:   return "Playback timed out";
    case PRU_DAC_ERR_PRU:       return "PRU reported an error";
    case PRU_DAC_ERR_HUNG:      return "PRU hung/crashed (heartbeat frozen)";
    case PRU_DAC_ERR_TOO_LARGE: return "More samples than the DDR buffer holds";
    default:                    return "Unknown error";
    }
}
