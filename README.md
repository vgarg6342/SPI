# PRU SPI Bit-Bang — BeagleBone Black

Bit-banged SPI master implementation using PRU0 on BeagleBone Black AM335x, supporting 4 chip select lines and up to ~10 MHz clock speed.

## Features

- **High-speed bit-bang**: ~10 MHz SPI via PRU0 at 200 MHz (5ns/cycle)
- **4 chip select lines**: CS0–CS3 independently controllable
- **All SPI modes**: Modes 0–3 (CPOL/CPHA combinations)
- **DRAM data transfer**: TX/RX data transferred via DDR with local SRAM staging
- **Clean C API**: Simple `init → transfer → close` workflow
- **Full-duplex**: Simultaneous TX and RX support

## Hardware Connections

```
BeagleBone Black P9 Header
──────────────────────────
P9_31 ──── SCLK  (SPI Clock)
P9_29 ──── MOSI  (Master Out, Slave In)
P9_30 ──── MISO  (Master In, Slave Out)
P9_28 ──── CS0   (Chip Select 0, active LOW)
P9_27 ──── CS1   (Chip Select 1, active LOW)
P9_25 ──── CS2   (Chip Select 2, active LOW)
P9_42 ──── CS3   (Chip Select 3, active LOW)
GND   ──── GND   (Common ground)
```

## Prerequisites

Install on the BeagleBone Black:

```bash
sudo apt-get update
sudo apt-get install -y \
    ti-pru-cgt-v2.3 \
    pru-software-support-package \
    build-essential
```

Disable HDMI/audio overlays (these pins conflict with PRU0 GPIO):

```bash
# Edit /boot/uEnv.txt and add/uncomment:
disable_uboot_overlay_video=1
disable_uboot_overlay_audio=1

# Reboot after changes
sudo reboot
```

## Quick Start

### 1. Build

```bash
cd /path/to/spi
make
```

### 2. Deploy (first time)

```bash
sudo ./scripts/deploy.sh
```

This will:
- Build PRU firmware and ARM code
- Install firmware to `/lib/firmware/`
- Configure pin muxing
- Start PRU via remoteproc

### 3. Run Example

```bash
# Basic demo (write, read, full-duplex on all CS lines)
sudo ./arm/pru_spi_example

# Loopback test (wire P9_29 MOSI → P9_30 MISO)
sudo ./arm/pru_spi_example --loopback

# Custom speed
sudo ./arm/pru_spi_example --speed 5000000

# SPI Mode 3
sudo ./arm/pru_spi_example --mode 3
```

## API Reference

### Initialization

```c
#include "pru_spi.h"

int ret = pru_spi_init();   // Load firmware, map memory
// ... use SPI ...
pru_spi_close();             // Cleanup
```

### SPI Transfers

```c
// Full-duplex: send and receive simultaneously
uint8_t tx[] = {0x9F, 0x00, 0x00};
uint8_t rx[3];
int n = pru_spi_transfer(0, tx, rx, 3, 0);  // CS0, 3 bytes, default timeout

// Write-only (MISO ignored)
pru_spi_write(1, tx, sizeof(tx), 0);  // CS1

// Read-only (sends 0x00 while reading)
pru_spi_read(2, rx, sizeof(rx), 0);   // CS2
```

### Configuration

```c
// Set SPI mode (0-3)
pru_spi_set_mode(0);       // CPOL=0, CPHA=0 (default)
pru_spi_set_mode(3);       // CPOL=1, CPHA=1

// Set clock speed (returns actual achievable frequency)
uint32_t actual = pru_spi_set_speed(10000000);  // 10 MHz
uint32_t actual = pru_spi_set_speed(1000000);   //  1 MHz
```

### Error Handling

```c
int ret = pru_spi_transfer(0, tx, rx, len, 5000);
if (ret < 0) {
    fprintf(stderr, "SPI error: %s\n", pru_spi_strerror(ret));
}
```

## Project Structure

```
spi/
├── include/
│   └── pru_spi_common.h      # Shared definitions (ARM & PRU)
├── pru/
│   ├── pru_spi_fw.c           # PRU0 firmware (bit-bang engine)
│   ├── resource_table.h       # Remoteproc resource table
│   ├── AM335x_PRU.cmd         # PRU linker script
│   └── Makefile               # PRU build (clpru)
├── arm/
│   ├── pru_spi.h              # Public C API header
│   ├── pru_spi.c              # API implementation
│   ├── pru_spi_example.c      # Example application
│   └── Makefile               # ARM build (gcc)
├── scripts/
│   ├── setup_pins.sh          # Pin muxing configuration
│   └── deploy.sh              # Build + deploy automation
├── Makefile                   # Top-level build
└── README.md                 # This file
```

## Architecture

### Data Flow

```
ARM Application
    │
    ▼
pru_spi API (pru_spi.c)
    │
    ├── Write TX data ──► DDR Buffer (TX)
    ├── Write command ──► PRU Shared RAM (cmd block)
    │                          │
    │                          ▼
    │                     PRU0 Firmware
    │                          │
    │                     ┌────┴────┐
    │                     │ Copy DDR │──► PRU Local SRAM (4KB staging)
    │                     │ to SRAM  │
    │                     └────┬────┘
    │                          │
    │                     ┌────┴────┐
    │                     │Bit-bang │──► SPI pins (R30/R31)
    │                     │from SRAM│    @ 10 MHz
    │                     └────┬────┘
    │                          │
    │                     ┌────┴────┐
    │                     │ Copy RX │──► DDR Buffer (RX)
    │                     │to DDR   │
    │                     └────┬────┘
    │                          │
    │                     Set status = DONE
    │                          │
    ├── Poll status ◄──────────┘
    ├── Read RX data ◄── DDR Buffer (RX)
    ▼
Return to application
```

### Memory Map

| Region | Physical Address | Size | Purpose |
|--------|-----------------|------|---------|
| PRU0 DRAM | `0x4A300000` | 8 KB | TX/RX staging buffers |
| PRU1 DRAM | `0x4A302000` | 8 KB | Reserved (for your other PRU use) |
| Shared RAM | `0x4A310000` | 12 KB | Command block + resource table |
| DDR Buffer | `0x9F000000` | 128 KB | TX (64KB) + RX (64KB) data buffers |

### Timing

| SPI Speed | Half-Period | `__delay_cycles` | Notes |
|-----------|-------------|-------------------|-------|
| 10 MHz | 50 ns (10 cycles) | 4 | Maximum practical speed |
| 5 MHz | 100 ns (20 cycles) | 14 | Good for most devices |
| 1 MHz | 500 ns (100 cycles) | 94 | Conservative, reliable |
| 250 KHz | 2 µs (400 cycles) | 200 | Minimum speed |

## Troubleshooting

### "cannot open /dev/mem"
Run as root: `sudo ./arm/pru_spi_example`

### "failed to start PRU"
- Check remoteproc: `ls /sys/class/remoteproc/`
- Check firmware: `ls -la /lib/firmware/am335x-pru0-fw`
- Check kernel log: `dmesg | tail -20`

### Pin conflicts
- Disable HDMI: `disable_uboot_overlay_video=1` in `/boot/uEnv.txt`
- Check pin state: `config-pin -q P9.31`
- Verify pin mode: `cat /sys/kernel/debug/pinctrl/44e10800.pinmux-pinctrl-single/pinmux-pins`

### PRU firmware doesn't respond
- Verify PRU is running: `cat /sys/class/remoteproc/remoteproc1/state`
- Check for correct remoteproc index (might be `remoteproc0` or `remoteproc2`)

## License

MIT License — Copyright (c) 2026
