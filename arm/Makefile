# ARM-side PRU SPI Library Makefile
#
# Builds the pru_spi library and example application.
# This code runs on the ARM Cortex-A8 of the BeagleBone Black.
#
# Usage:
#   make              # Build library and example
#   make clean        # Remove build artifacts
#
# Cross-compilation:
#   make CC=arm-linux-gnueabihf-gcc
#
# Copyright (c) 2026 — MIT License

# -----------------------------------------------------------------------
# Compiler Settings
# -----------------------------------------------------------------------
CC       ?= gcc
CFLAGS    = -Wall -Wextra -Wno-unused-parameter -O2 \
            -I../include -I.
LDFLAGS   = -lrt    # For clock_gettime (CLOCK_MONOTONIC)

# -----------------------------------------------------------------------
# Sources and Targets
# -----------------------------------------------------------------------
LIB_SRC    = pru_spi.c
LIB_OBJ    = $(LIB_SRC:.c=.o)
LIB_TARGET = libpru_spi.a

EXAMPLE_SRC    = pru_spi_example.c
EXAMPLE_OBJ    = $(EXAMPLE_SRC:.c=.o)
EXAMPLE_TARGET = pru_spi_example

# -----------------------------------------------------------------------
# Build Rules
# -----------------------------------------------------------------------
.PHONY: all clean

all: $(LIB_TARGET) $(EXAMPLE_TARGET)

# Build static library
$(LIB_TARGET): $(LIB_OBJ)
	ar rcs $@ $^
	@echo "Built static library: $@"

# Build example application (links against library)
$(EXAMPLE_TARGET): $(EXAMPLE_OBJ) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(EXAMPLE_OBJ) -L. -lpru_spi $(LDFLAGS)
	@echo "Built example: $@"

# Compile C sources
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Dependencies
pru_spi.o: pru_spi.c pru_spi.h ../include/pru_spi_common.h
pru_spi_example.o: pru_spi_example.c pru_spi.h

# Clean build artifacts
clean:
	@echo "Cleaning ARM build artifacts..."
	rm -f $(LIB_OBJ) $(EXAMPLE_OBJ) $(LIB_TARGET) $(EXAMPLE_TARGET)
	@echo "Done."
