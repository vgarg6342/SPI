#!/bin/bash
#
# deploy.sh — Build, deploy, and start PRU SPI firmware
#
# This script automates the full deployment workflow:
#   1. Stop PRU if running
#   2. Build PRU firmware and ARM code
#   3. Install firmware to /lib/firmware/
#   4. Configure pins for PRU I/O
#   5. Start PRU via remoteproc
#
# Usage:
#   sudo ./scripts/deploy.sh              # Full deploy
#   sudo ./scripts/deploy.sh --build-only # Build without deploy
#   sudo ./scripts/deploy.sh --no-build   # Deploy without building
#
# Copyright (c) 2026 — MIT License

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# -----------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------
PRU_FW_NAME="am335x-pru0-fw"
FW_INSTALL_DIR="/lib/firmware"
REMOTEPROC_PATH="/sys/class/remoteproc/remoteproc1"

# Parse flags
BUILD=1
DEPLOY=1
for arg in "$@"; do
    case "$arg" in
        --build-only) DEPLOY=0 ;;
        --no-build)   BUILD=0 ;;
        --help)
            echo "Usage: $0 [--build-only|--no-build|--help]"
            exit 0
            ;;
    esac
done

# -----------------------------------------------------------------------
# Check prerequisites
# -----------------------------------------------------------------------
echo "=== PRU SPI Deployment ==="
echo "Project: $PROJECT_DIR"
echo ""

if [ "$DEPLOY" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Deployment requires root privileges."
    echo "Run with: sudo $0"
    exit 1
fi

# -----------------------------------------------------------------------
# Step 1: Stop PRU if running
# -----------------------------------------------------------------------
if [ "$DEPLOY" -eq 1 ]; then
    echo "[1/5] Stopping PRU..."
    if [ -f "$REMOTEPROC_PATH/state" ]; then
        STATE=$(cat "$REMOTEPROC_PATH/state" 2>/dev/null || echo "unknown")
        if [ "$STATE" = "running" ]; then
            echo "stop" > "$REMOTEPROC_PATH/state" 2>/dev/null || true
            sleep 0.2
            echo "  PRU stopped."
        else
            echo "  PRU is already stopped (state: $STATE)."
        fi
    else
        echo "  WARNING: remoteproc path not found at $REMOTEPROC_PATH"
        echo "  Trying remoteproc0..."
        REMOTEPROC_PATH="/sys/class/remoteproc/remoteproc0"
        if [ -f "$REMOTEPROC_PATH/state" ]; then
            echo "  Found at $REMOTEPROC_PATH"
            STATE=$(cat "$REMOTEPROC_PATH/state" 2>/dev/null || echo "unknown")
            if [ "$STATE" = "running" ]; then
                echo "stop" > "$REMOTEPROC_PATH/state" 2>/dev/null || true
                sleep 0.2
            fi
        fi
    fi
else
    echo "[1/5] Skipping PRU stop (build-only mode)."
fi

# -----------------------------------------------------------------------
# Step 2: Build PRU firmware
# -----------------------------------------------------------------------
if [ "$BUILD" -eq 1 ]; then
    echo ""
    echo "[2/5] Building PRU firmware..."
    cd "$PROJECT_DIR/pru"
    make clean
    make
    echo "  PRU firmware built: $PROJECT_DIR/pru/$PRU_FW_NAME"
else
    echo ""
    echo "[2/5] Skipping PRU build (--no-build)."
fi

# -----------------------------------------------------------------------
# Step 3: Build ARM code
# -----------------------------------------------------------------------
if [ "$BUILD" -eq 1 ]; then
    echo ""
    echo "[3/5] Building ARM code..."
    cd "$PROJECT_DIR/arm"
    make clean
    make
    echo "  ARM code built: $PROJECT_DIR/arm/dac_load"
else
    echo ""
    echo "[3/5] Skipping ARM build (--no-build)."
fi

if [ "$DEPLOY" -eq 0 ]; then
    echo ""
    echo "=== Build complete (deploy skipped) ==="
    exit 0
fi

# -----------------------------------------------------------------------
# Step 4: Install firmware and configure pins
# -----------------------------------------------------------------------
echo ""
echo "[4/5] Installing firmware and configuring pins..."

# Copy firmware
if [ -f "$PROJECT_DIR/pru/$PRU_FW_NAME" ]; then
    cp "$PROJECT_DIR/pru/$PRU_FW_NAME" "$FW_INSTALL_DIR/$PRU_FW_NAME"
    echo "  Firmware installed to $FW_INSTALL_DIR/$PRU_FW_NAME"
else
    echo "  ERROR: Firmware file not found at $PROJECT_DIR/pru/$PRU_FW_NAME"
    exit 1
fi

# Configure pins
echo "  Configuring pins..."
"$SCRIPT_DIR/setup_pins.sh"

# -----------------------------------------------------------------------
# Step 5: Start PRU
# -----------------------------------------------------------------------
echo ""
echo "[5/5] Starting PRU..."

# Set firmware name
echo "$PRU_FW_NAME" > "$REMOTEPROC_PATH/firmware"
echo "  Firmware set to: $PRU_FW_NAME"

# Start PRU
echo "start" > "$REMOTEPROC_PATH/state"
sleep 0.5

# Verify
STATE=$(cat "$REMOTEPROC_PATH/state" 2>/dev/null || echo "unknown")
if [ "$STATE" = "running" ]; then
    echo "  PRU is running!"
else
    echo "  WARNING: PRU state is '$STATE' (expected 'running')"
    echo "  Check: dmesg | tail -20"
fi

# -----------------------------------------------------------------------
# Done
# -----------------------------------------------------------------------
echo ""
echo "=== Deployment complete ==="
echo ""
echo "Generate a test waveform (on the dev PC or the board):"
echo "  python3 $PROJECT_DIR/scripts/gen_adc.py --out adc.txt"
echo ""
echo "To play it (single DAC on P9_31/P9_29/P9_28):"
echo "  sudo $PROJECT_DIR/arm/dac_load --file adc.txt"
echo ""
echo "To check the PRU is alive (heartbeat):"
echo "  sudo $PROJECT_DIR/arm/dac_load --status"
echo "  cat $REMOTEPROC_PATH/state"
echo "  dmesg | tail -10"
