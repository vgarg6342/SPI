# PRU SPI Firmware Makefile
#
# Builds the PRU0 bit-banged SPI firmware using TI PRU Code Generation Tools.
#
# Prerequisites:
#   sudo apt-get install ti-pru-cgt-v2.3 pru-software-support-package
#
# Usage:
#   make              # Build firmware
#   make clean        # Remove build artifacts
#   make install      # Copy firmware to /lib/firmware/
#
# Copyright (c) 2026 — MIT License

# -----------------------------------------------------------------------
# Tool Paths
#
# Adjust these if your PRU CGT or software support package is installed
# in a non-standard location.
# -----------------------------------------------------------------------
PRU_CGT     ?= /usr/share/ti/cgt-pru
PRU_SWPKG   ?= /usr/lib/ti/pru-software-support-package

# Compiler and linker
CC           = clpru
LD           = lnkpru

# -----------------------------------------------------------------------
# Compiler Flags
#
# -v3               : PRU architecture version 3 (AM335x)
# -O2               : Optimization level 2
# --endian=little   : Little-endian byte order
# --hardware_mac=on : Enable hardware multiply-accumulate
# --display_error_number : Show error numbers for easier debugging
# -ppd              : Generate dependency files
# -ppa              : Append source file to dependency output
# -----------------------------------------------------------------------
CFLAGS  = -v3 -O2 --display_error_number --endian=little \
          --hardware_mac=on -ppd -ppa \
          --include_path=$(PRU_CGT)/include \
          --include_path=$(PRU_SWPKG)/include \
          --include_path=$(PRU_SWPKG)/include/am335x \
          --include_path=../include

# -----------------------------------------------------------------------
# Linker Flags
# -----------------------------------------------------------------------
LDFLAGS = AM335x_PRU.cmd \
          --search_path=$(PRU_CGT)/lib \
          --library=libc.a

# -----------------------------------------------------------------------
# Source and Target
# -----------------------------------------------------------------------
TARGET   = am335x-pru0-fw
SOURCES  = pru_spi_fw.c
OBJECTS  = $(SOURCES:.c=.object)

# -----------------------------------------------------------------------
# Build Rules
# -----------------------------------------------------------------------
.PHONY: all clean install

all: $(TARGET)

# Compile C source to object file
%.object: %.c
	$(CC) $(CFLAGS) -fe $@ $<

# Link object file to produce firmware
$(TARGET): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^

# Install firmware to the standard location for remoteproc
install: $(TARGET)
	@echo "Installing firmware to /lib/firmware/$(TARGET)"
	sudo cp $(TARGET) /lib/firmware/$(TARGET)
	@echo "Done. Use remoteproc to load the firmware."

# Clean build artifacts
clean:
	@echo "Cleaning PRU build artifacts..."
	rm -f $(TARGET) $(OBJECTS) *.pp *.d
	@echo "Done."
