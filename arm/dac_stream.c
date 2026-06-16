/*
 * dac_stream.c — Stream an adc.txt sine table to 4 DACs at a fixed sample rate
 *
 * Reads a whitespace/comma-separated table (one sample per line, 4 columns =
 * DAC0..DAC3, each a 12-bit code 0..4095), then streams it to the PRU's
 * shared-RAM ring buffer. The PRU paces output off its IEP timer (default
 * 10,000 samples/s) and shifts each 16-bit frame out to all 4 DACs at once on
 * SPI mode 0 @ 1 MHz — no DDR involved.
 *
 * Robustness:
 *   - mlockall() (and optional SCHED_FIFO) so the producer is not starved.
 *   - Backpressure in pru_dac_stream_push(): data is never dropped.
 *   - Out-of-range values are clamped to 12 bits with a warning.
 *   - Reports frames played and ring underflows at the end.
 *
 * Usage:
 *   sudo ./dac_stream [--file adc.txt] [--rate 10000] [--count N] [--rt]
 *
 *   --file   F   input table (default: adc.txt)
 *   --rate   N   samples per second (default: 10000)
 *   --count  N   play only the first N samples (default: whole file)
 *   --rt         request SCHED_FIFO real-time priority (needs root)
 *
 * Copyright (c) 2026 — MIT License
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE          /* sched_setscheduler(), mlockall() */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>

#include "pru_spi.h"
#include "pru_spi_common.h"

#define DEFAULT_FILE   "adc.txt"
#define MAX_SAMPLES    (4 * 1024 * 1024)   /* generous host-side cap */

static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* -----------------------------------------------------------------------
 * Parse adc.txt into a flat array of 4-wide samples.
 *
 * Each non-blank, non-'#' line must contain 4 integers (DAC0..DAC3). Values are
 * clamped to 0..DAC_VALUE_MAX (12-bit). Returns sample count, or -1 on error.
 * *out is malloc'd as [count][4] uint16; caller frees.
 * ----------------------------------------------------------------------- */
static long load_table(const char *path, uint16_t (**out)[4])
{
    FILE *fp;
    char line[256];
    long cap = 4096, count = 0;
    uint16_t (*buf)[4];
    unsigned long warned = 0;
    unsigned long lineno = 0;

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "dac_stream: cannot open '%s'\n", path);
        return -1;
    }

    buf = malloc((size_t)cap * sizeof(*buf));
    if (!buf) {
        fclose(fp);
        fprintf(stderr, "dac_stream: out of memory\n");
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        long v[4];
        int i, n;

        lineno++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;

        /* Accept whitespace or commas between the four columns. */
        for (i = 0; line[i] != '\0'; i++)
            if (line[i] == ',')
                line[i] = ' ';

        n = sscanf(line, "%ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3]);
        if (n != 4) {
            fprintf(stderr, "dac_stream: line %lu: expected 4 values, got %d\n",
                    lineno, n);
            free(buf);
            fclose(fp);
            return -1;
        }

        if (count >= cap) {
            long ncap = cap * 2;
            uint16_t (*nb)[4];
            if (ncap > MAX_SAMPLES) {
                fprintf(stderr, "dac_stream: too many samples (> %d)\n",
                        MAX_SAMPLES);
                free(buf);
                fclose(fp);
                return -1;
            }
            nb = realloc(buf, (size_t)ncap * sizeof(*buf));
            if (!nb) {
                fprintf(stderr, "dac_stream: out of memory\n");
                free(buf);
                fclose(fp);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }

        for (i = 0; i < 4; i++) {
            long x = v[i];
            if (x < 0)
                x = 0;
            if (x > (long)DAC_VALUE_MAX) {
                x = DAC_VALUE_MAX;        /* 12-bit overflow guard */
                warned++;
            }
            buf[count][i] = (uint16_t)x;
        }
        count++;
    }

    fclose(fp);

    if (warned)
        fprintf(stderr, "dac_stream: clamped %lu out-of-range value(s) to %u\n",
                warned, DAC_VALUE_MAX);

    *out = buf;
    return count;
}

static void try_realtime(void)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "dac_stream: SCHED_FIFO request failed "
                        "(continuing at normal priority)\n");
    else
        printf("dac_stream: running at SCHED_FIFO priority %d\n",
               sp.sched_priority);
}

int main(int argc, char *argv[])
{
    const char *file = DEFAULT_FILE;
    uint32_t rate = DAC_SAMPLE_RATE_HZ;
    long count = -1;            /* -1 = whole file */
    int want_rt = 0;
    int i, ret;
    uint16_t (*table)[4] = NULL;
    long total, n;
    int underflows;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file = argv[++i];
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--rt") == 0) {
            want_rt = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--file adc.txt] [--rate 10000] "
                   "[--count N] [--rt]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "dac_stream: unknown arg '%s' (try --help)\n",
                    argv[i]);
            return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Lock memory so page faults can't stall the producer mid-stream. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "dac_stream: mlockall failed (continuing)\n");
    if (want_rt)
        try_realtime();

    total = load_table(file, &table);
    if (total < 0)
        return 1;
    if (total == 0) {
        fprintf(stderr, "dac_stream: '%s' has no samples\n", file);
        free(table);
        return 1;
    }
    if (count < 0 || count > total)
        count = total;

    printf("dac_stream: %ld samples from '%s', streaming %ld at %u Hz "
           "(%.3f s)\n", total, file, count, rate, (double)count / (double)rate);

    ret = pru_spi_init();
    if (ret != PRU_SPI_OK) {
        fprintf(stderr, "FATAL: %s\n", pru_spi_strerror(ret));
        free(table);
        return 1;
    }

    ret = pru_dac_stream_start(rate);
    if (ret != PRU_SPI_OK) {
        fprintf(stderr, "dac_stream: start failed: %s\n", pru_spi_strerror(ret));
        pru_spi_close();
        free(table);
        return 1;
    }

    for (n = 0; n < count && g_running; n++) {
        ret = pru_dac_stream_push(table[n]);
        if (ret != PRU_SPI_OK) {
            fprintf(stderr, "dac_stream: push failed at sample %ld: %s\n",
                    n, pru_spi_strerror(ret));
            break;
        }
    }

    underflows = pru_dac_stream_end(0);
    if (underflows < 0)
        fprintf(stderr, "dac_stream: end failed: %s\n",
                pru_spi_strerror(underflows));
    else
        printf("dac_stream: done — %ld samples queued, %d underflow(s)\n",
               n, underflows);

    pru_spi_close();
    free(table);
    return 0;
}
