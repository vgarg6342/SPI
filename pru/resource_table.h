/*
 * resource_table.h — Empty resource table for PRU remoteproc
 *
 * The remoteproc driver requires a resource table to load firmware.
 * Since we communicate via direct shared memory (no RPMsg), we use
 * a minimal empty resource table.
 *
 * Copyright (c) 2026 — MIT License
 */

#ifndef RESOURCE_TABLE_H
#define RESOURCE_TABLE_H

#include <stdint.h>
#include <pru_types.h>

/*
 * Minimal resource table with no entries.
 * Required by remoteproc even when no resources are needed.
 */
struct my_resource_table {
    struct resource_table base;
    uint32_t offset[1];     /* Must have at least 1 entry for alignment */
};

#pragma DATA_SECTION(pru_remoteproc_ResourceTable, ".resource_table")
#pragma RETAIN(pru_remoteproc_ResourceTable)
struct my_resource_table pru_remoteproc_ResourceTable = {
    .base = {
        .ver = 1,           /* Resource table version: must be 1 */
        .num = 0,           /* Number of entries: 0 (empty) */
        .reserved = { 0, 0 },
    },
    .offset = { 0 },
};

#endif /* RESOURCE_TABLE_H */
