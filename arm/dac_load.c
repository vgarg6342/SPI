/*
 * dac_load.c — the one and only example: play adc.txt out of a single DAC
 *
 * Reads adc.txt (one 12-bit DAC code, 0..4095, per line), then streams it to
 * the PRU which copies it out of DDR and shifts it to one SPI DAC at a fixed
 * sample rate (SPI mode 0, 1 MHz bit clock, DAC_SAMPLE_RATE_HZ sample rate —
 * both compile-time). How many samples are played is capped by the transmission
 * duration (STREAM_DURATION_SEC), overridable with --duration.
 *
 * Usage:
 *   sudo ./dac_load                         # adc.txt, default duration
 *   sudo ./dac_load --file wave.txt
 *   sudo ./dac_load --duration 5
 *   sudo ./dac_load --status                # just report PRU liveness and exit
 *
 * adc.txt format (whitespace/comment tolerant):
 *   # comments and blank lines are ignored
 *   2048
 *   2148
 *   ...
 *
 * Copyright (c) 2026 — MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>

#include "pru_spi.h"
#include "pru_spi_common.h"

#define DEFAULT_FILE  "adc.txt"

static volatile int g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* Load one 12-bit code per line into a malloc'd array. Returns count, or -1. */
static long load_codes(const char *path, uint16_t **out)
{
    FILE *fp = fopen(path, "r");
    char line[128];
    long cap = 4096, count = 0;
    unsigned long lineno = 0, clamped = 0;
    uint16_t *buf;

    if (!fp) {
        fprintf(stderr, "dac_load: cannot open '%s'\n", path);
        return -1;
    }
    buf = malloc((size_t)cap * sizeof(*buf));
    if (!buf) {
        fclose(fp);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        long v;

        lineno++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;

        if (sscanf(p, "%ld", &v) != 1) {
            fprintf(stderr, "dac_load: line %lu: not a number\n", lineno);
            free(buf);
            fclose(fp);
            return -1;
        }
        if (v < 0)            { v = 0;             clamped++; }
        if (v > DAC_VALUE_MAX){ v = DAC_VALUE_MAX; clamped++; }

        if (count >= cap) {
            uint16_t *nb = realloc(buf, (size_t)(cap * 2) * sizeof(*buf));
            if (!nb) {
                free(buf);
                fclose(fp);
                return -1;
            }
            buf = nb;
            cap *= 2;
        }
        buf[count++] = (uint16_t)v;
    }
    fclose(fp);

    if (clamped)
        fprintf(stderr, "dac_load: clamped %lu out-of-range value(s) to 0..%u\n",
                clamped, (unsigned)DAC_VALUE_MAX);
    *out = buf;
    return count;
}

int main(int argc, char *argv[])
{
    const char *file = DEFAULT_FILE;
    const uint32_t rate = DAC_SAMPLE_RATE_HZ;   /* fixed at compile time */
    uint32_t duration = STREAM_DURATION_SEC;
    int status_only = 0;
    int i, ret;
    uint16_t *codes = NULL;
    long total;
    uint32_t play_n;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) {
            file = argv[++i];
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            duration = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--status")) {
            status_only = 1;
        } else if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--file adc.txt] [--duration %u] [--status]\n"
                   "  (sample rate is fixed at %u Hz at compile time)\n",
                   argv[0], (unsigned)STREAM_DURATION_SEC,
                   (unsigned)DAC_SAMPLE_RATE_HZ);
            return 0;
        } else {
            fprintf(stderr, "dac_load: unknown arg '%s' (try --help)\n", argv[i]);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* --status: bring the PRU up, report liveness, leave. */
    if (status_only) {
        char state[32] = "?";
        ret = pru_dac_init();
        if (ret != PRU_DAC_OK) {
            fprintf(stderr, "dac_load: %s\n", pru_dac_strerror(ret));
            return 1;
        }
        pru_dac_get_state(state, sizeof(state));
        printf("dac_load: remoteproc state '%s', PRU %s\n",
               state, pru_dac_is_alive() == 1 ? "ALIVE" : "HUNG/DEAD");
        pru_dac_close();
        return 0;
    }

    total = load_codes(file, &codes);
    if (total < 0)
        return 1;
    if (total == 0) {
        fprintf(stderr, "dac_load: '%s' has no samples\n", file);
        free(codes);
        return 1;
    }

    /* Cap the run to the transmission duration: play_n = min(file, rate*dur). */
    play_n = (uint32_t)total;
    if (duration > 0 && (uint64_t)rate * duration < play_n)
        play_n = rate * duration;
    if (play_n > pru_dac_capacity_samples()) {
        play_n = pru_dac_capacity_samples();
        fprintf(stderr, "dac_load: capped to DDR capacity %u samples "
                "(raise STREAM_DURATION_SEC and rebuild for more)\n", play_n);
    }

    printf("dac_load: %ld samples in '%s'; playing %u at %u Hz fixed (%.3f s)\n",
           total, file, play_n, rate, (double)play_n / (double)rate);

    ret = pru_dac_init();
    if (ret != PRU_DAC_OK) {
        fprintf(stderr, "dac_load: %s\n", pru_dac_strerror(ret));
        free(codes);
        return 1;
    }

    ret = pru_dac_play(codes, play_n, 0);
    if (ret < 0) {
        fprintf(stderr, "dac_load: play failed: %s\n", pru_dac_strerror(ret));
        pru_dac_close();
        free(codes);
        return 1;
    }
    printf("dac_load: done — %d samples played\n", ret);

    pru_dac_close();
    free(codes);
    return 0;
}
