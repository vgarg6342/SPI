# ARM-side Makefile — PRU single-DAC player
#
# Builds the pru_spi library and the one example, dac_load.
# Runs on the BeagleBone Black ARM (Cortex-A8). Build ON the board.
#
# Usage:
#   make           # build libpru_spi.a + dac_load
#   make clean
#
# Copyright (c) 2026 — MIT License

CC      ?= gcc
CFLAGS   = -Wall -Wextra -Wno-unused-parameter -O2 -I../include -I.
LDFLAGS  = -lrt           # clock_gettime(CLOCK_MONOTONIC)

LIB_SRC    = pru_spi.c
LIB_OBJ    = $(LIB_SRC:.c=.o)
LIB_TARGET = libpru_spi.a

APP_SRC    = dac_load.c
APP_OBJ    = $(APP_SRC:.c=.o)
APP_TARGET = dac_load

.PHONY: all clean

all: $(LIB_TARGET) $(APP_TARGET)

$(LIB_TARGET): $(LIB_OBJ)
	ar rcs $@ $^
	@echo "Built static library: $@"

$(APP_TARGET): $(APP_OBJ) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJ) -L. -lpru_spi $(LDFLAGS)
	@echo "Built example: $@"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

pru_spi.o: pru_spi.c pru_spi.h ../include/pru_spi_common.h
dac_load.o: dac_load.c pru_spi.h ../include/pru_spi_common.h

clean:
	@echo "Cleaning ARM build artifacts..."
	rm -f $(LIB_OBJ) $(APP_OBJ) $(LIB_TARGET) $(APP_TARGET)
	@echo "Done."
