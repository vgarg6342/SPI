# Top-Level Makefile — PRU SPI Bit-Bang for BeagleBone Black
#
# Builds both PRU firmware and ARM-side library/example.
#
# Usage:
#   make              # Build everything
#   make pru          # Build PRU firmware only
#   make arm          # Build ARM code only
#   make deploy       # Build and deploy (requires sudo)
#   make clean        # Clean all build artifacts
#
# Copyright (c) 2026 — MIT License

.PHONY: all pru arm deploy clean help

all: pru arm

pru:
	@echo "=== Building PRU firmware ==="
	$(MAKE) -C pru

arm:
	@echo "=== Building ARM code ==="
	$(MAKE) -C arm

deploy: all
	@echo "=== Deploying ==="
	sudo ./scripts/deploy.sh --no-build

clean:
	@echo "=== Cleaning all ==="
	$(MAKE) -C pru clean
	$(MAKE) -C arm clean

help:
	@echo "PRU SPI Bit-Bang — Build Targets:"
	@echo ""
	@echo "  make          Build everything (PRU firmware + ARM code)"
	@echo "  make pru      Build PRU firmware only"
	@echo "  make arm      Build ARM code only"
	@echo "  make deploy   Build and deploy to BeagleBone (needs sudo)"
	@echo "  make clean    Remove all build artifacts"
	@echo "  make help     Show this help message"
	@echo ""
	@echo "Prerequisites:"
	@echo "  PRU: ti-pru-cgt, pru-software-support-package"
	@echo "  ARM: gcc (native or arm-linux-gnueabihf-gcc for cross)"
