#!/bin/bash
#
# setup_pins.sh — Mux the BeagleBone Black pins for the PRU single-DAC player
#
# Three PRU0 outputs drive one SPI DAC (mode 0). No MISO (DACs are write-only).
#
#   Signal | R30 bit | Header pin | DAC pin
#   -------|---------|------------|------------------
#   SCLK   | bit 0   | P9_31      | SCK
#   SDI    | bit 1   | P9_29      | SDI / DIN
#   CS     | bit 3   | P9_28      | CS / SYNC (active LOW)
#
# Tie the DAC's LDAC pin LOW so the output updates when CS rises.
#
# IMPORTANT: if the HDMI/audio overlay is enabled it steals some P8/P9 pins.
# Disable it in /boot/uEnv.txt (see USER_MANUAL.md §3):
#   disable_uboot_overlay_video=1
#   disable_uboot_overlay_audio=1
#
# Must be run as root (sudo).
#
# Copyright (c) 2026 — MIT License

set -e

echo "=== PRU DAC pin configuration ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root (sudo)"
    exit 1
fi

if ! command -v config-pin &> /dev/null; then
    echo "ERROR: config-pin not found. Install: sudo apt-get install bb-cape-overlays"
    exit 1
fi

config-pin P9.31 pruout && echo "  P9_31 -> SCLK (pruout)"
config-pin P9.29 pruout && echo "  P9_29 -> SDI  (pruout)"
config-pin P9.28 pruout && echo "  P9_28 -> CS   (pruout, active LOW)"

echo ""
echo "Verifying:"
for pin in P9.31 P9.29 P9.28; do
    state=$(config-pin -q "$pin" 2>/dev/null || echo "unknown")
    echo "  $pin: $state"
done

echo ""
echo "Done. Pins ready for the PRU DAC player."
