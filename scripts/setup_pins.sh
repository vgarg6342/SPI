#!/bin/bash
#
# setup_pins.sh — Configure BeagleBone Black pins for PRU0 SPI
#
# Uses config-pin utility to set pin muxing for PRU0 direct I/O.
# Must be run as root (or with sudo).
#
# Pin Mapping:
#   P9_31 → SCLK  (PRU0 R30 bit 0, output)
#   P9_29 → MOSI  (PRU0 R30 bit 1, output)
#   P9_30 → MISO  (PRU0 R31 bit 2, input)
#   P9_28 → CS0   (PRU0 R30 bit 3, output)
#   P9_27 → CS1   (PRU0 R30 bit 5, output)
#   P9_25 → CS2   (PRU0 R30 bit 7, output)
#   P9_42 → CS3   (PRU0 R30 bit 4, output)
#
# IMPORTANT: If using HDMI/audio overlay, disable it in /boot/uEnv.txt:
#   disable_uboot_overlay_video=1
#   disable_uboot_overlay_audio=1
#
# Copyright (c) 2026 — MIT License

set -e

echo "=== PRU SPI Pin Configuration ==="

# Check for root privileges
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This script must be run as root (sudo)"
    exit 1
fi

# Check for config-pin utility
if ! command -v config-pin &> /dev/null; then
    echo "ERROR: config-pin utility not found."
    echo "Install with: sudo apt-get install bb-cape-overlays"
    exit 1
fi

echo "Configuring SPI output pins..."

# SCLK — P9_31 as PRU output
config-pin P9.31 pruout
echo "  P9_31 → SCLK (pruout)"

# MOSI — P9_29 as PRU output
config-pin P9.29 pruout
echo "  P9_29 → MOSI (pruout)"

# MISO — P9_30 as PRU input
config-pin P9.30 pruin
echo "  P9_30 → MISO (pruin)"

# CS0 — P9_28 as PRU output
config-pin P9.28 pruout
echo "  P9_28 → CS0  (pruout)"

# CS1 — P9_27 as PRU output
config-pin P9.27 pruout
echo "  P9_27 → CS1  (pruout)"

# CS2 — P9_25 as PRU output
config-pin P9.25 pruout
echo "  P9_25 → CS2  (pruout)"

# CS3 — P9_42 as PRU output
config-pin P9.42 pruout
echo "  P9_42 → CS3  (pruout)"

echo ""
echo "=== Pin configuration complete ==="

# Verify configuration
echo ""
echo "Verifying pin states:"
for pin in P9.31 P9.29 P9.30 P9.28 P9.27 P9.25 P9.42; do
    state=$(config-pin -q $pin 2>/dev/null || echo "unknown")
    echo "  $pin: $state"
done

echo ""
echo "Done. Pins are ready for PRU SPI."
