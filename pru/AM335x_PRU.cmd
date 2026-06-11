/*
 * AM335x_PRU.cmd — Linker command file for PRU0 on AM335x (BeagleBone Black)
 *
 * Defines the memory layout and section placement for PRU0 firmware.
 *
 * Memory Map (from PRU's perspective):
 *   Instruction RAM:  8KB  at 0x00000000
 *   Data RAM 0 (local): 8KB at 0x00000000 (data bus)
 *   Data RAM 1 (peer):  8KB at 0x00002000 (data bus)
 *   Shared RAM:        12KB at 0x00010000 (data bus)
 *   DDR:               via OCP master port (external)
 *
 * Copyright (c) 2026 — MIT License
 */

-cr  /* Link using C conventions (RAM-based autoinitialization) */
-stack 0x100
-heap 0x100

MEMORY
{
    PAGE 0:  /* Program Memory */
        PRU_IMEM    : org = 0x00000000, len = 0x00002000  /* 8KB instruction RAM */

    PAGE 1:  /* Data Memory */
        PRU_DMEM_0_1 : org = 0x00000000, len = 0x00002000 /* 8KB PRU0 local data RAM */
        PRU_DMEM_1_0 : org = 0x00002000, len = 0x00002000 /* 8KB PRU1 data RAM (peer) */
        PRU_SHAREDMEM: org = 0x00010000, len = 0x00003000 /* 12KB shared RAM */
}

SECTIONS
{
    /* Code sections → instruction memory */
    .text          : > PRU_IMEM,    PAGE 0

    /* Data sections → PRU0 local data RAM */
    .stack         : > PRU_DMEM_0_1, PAGE 1
    .bss           : > PRU_DMEM_0_1, PAGE 1
    .cio           : > PRU_DMEM_0_1, PAGE 1
    .data          : > PRU_DMEM_0_1, PAGE 1
    .switch        : > PRU_DMEM_0_1, PAGE 1
    .sysmem        : > PRU_DMEM_0_1, PAGE 1
    .cinit         : > PRU_DMEM_0_1, PAGE 1
    .rodata        : > PRU_DMEM_0_1, PAGE 1
    .rofardata     : > PRU_DMEM_0_1, PAGE 1
    .farbss        : > PRU_DMEM_0_1, PAGE 1
    .fardata       : > PRU_DMEM_0_1, PAGE 1

    /* Resource table → shared RAM (remoteproc looks for this) */
    .resource_table : > PRU_SHAREDMEM, PAGE 1

    /* Shared data section for command block */
    .shared        : > PRU_SHAREDMEM, PAGE 1
}
