#!/bin/bash
#
# sync_to_bbb.sh — Push this project to the BeagleBone for on-device build
#
# Builds happen ON the BeagleBone (clpru + native gcc live there), not on the
# dev machine. This script rsyncs the source tree to the BeagleBone and,
# optionally, runs the build there over SSH.
#
# Destination on the BeagleBone: /home/debian/DERIC_testing
#
# Usage:
#   ./scripts/sync_to_bbb.sh                 # sync only
#   ./scripts/sync_to_bbb.sh --build         # sync, then `make` on the BBB
#   ./scripts/sync_to_bbb.sh --deploy        # sync, `make`, then deploy (sudo)
#   BBB_HOST=beaglebone.local ./scripts/sync_to_bbb.sh
#
# Override defaults via environment:
#   BBB_USER  (default: debian)
#   BBB_HOST  (default: 192.168.7.2  — standard BBB USB-network address)
#   BBB_DEST  (default: /home/debian/DERIC_testing)
#
# Copyright (c) 2026 — MIT License

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# -----------------------------------------------------------------------
# Configuration (override via environment)
# -----------------------------------------------------------------------
BBB_USER="${BBB_USER:-debian}"
BBB_HOST="${BBB_HOST:-192.168.7.2}"
BBB_DEST="${BBB_DEST:-/home/debian/DERIC_testing}"

DO_BUILD=0
DO_DEPLOY=0
for arg in "$@"; do
    case "$arg" in
        --build)  DO_BUILD=1 ;;
        --deploy) DO_BUILD=1; DO_DEPLOY=1 ;;
        --help)
            echo "Usage: $0 [--build|--deploy|--help]"
            echo "  --build   sync then run 'make' on the BeagleBone"
            echo "  --deploy  sync, build, then 'sudo ./scripts/deploy.sh' on the BeagleBone"
            echo ""
            echo "Env: BBB_USER (debian), BBB_HOST (192.168.7.2), BBB_DEST ($BBB_DEST)"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg (try --help)"
            exit 1
            ;;
    esac
done

TARGET="${BBB_USER}@${BBB_HOST}"

echo "=== Sync to BeagleBone ==="
echo "  From: $PROJECT_DIR"
echo "  To:   ${TARGET}:${BBB_DEST}"
echo ""

# -----------------------------------------------------------------------
# Ensure the destination directory exists on the BeagleBone
# -----------------------------------------------------------------------
ssh "$TARGET" "mkdir -p '$BBB_DEST'"

# -----------------------------------------------------------------------
# Rsync the source tree (exclude VCS metadata and build artifacts)
# -----------------------------------------------------------------------
rsync -avz --delete \
    --exclude '.git/' \
    --exclude '.claude/' \
    --exclude '*.o' \
    --exclude '*.a' \
    --exclude '*.lst' \
    --exclude '*.map' \
    --exclude 'gen/' \
    --exclude 'am335x-pru0-fw' \
    --exclude 'arm/pru_spi_example' \
    "$PROJECT_DIR/" "${TARGET}:${BBB_DEST}/"

echo ""
echo "  Source synced."

# -----------------------------------------------------------------------
# Optional: build (and deploy) on the BeagleBone over SSH
# -----------------------------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    echo ""
    echo "=== Building on the BeagleBone ==="
    ssh "$TARGET" "cd '$BBB_DEST' && make"
fi

if [ "$DO_DEPLOY" -eq 1 ]; then
    echo ""
    echo "=== Deploying on the BeagleBone (sudo) ==="
    ssh -t "$TARGET" "cd '$BBB_DEST' && sudo ./scripts/deploy.sh --no-build"
fi

echo ""
echo "=== Done ==="
if [ "$DO_BUILD" -eq 0 ]; then
    echo "Next, on the BeagleBone:"
    echo "  ssh ${TARGET}"
    echo "  cd ${BBB_DEST} && make && sudo ./scripts/deploy.sh"
fi
