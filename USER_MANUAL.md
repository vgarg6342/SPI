# User Manual — PRU SPI Bit-Bang on BeagleBone Black

This manual walks through setting up, building, deploying, and using the
PRU0 bit-banged SPI driver on a BeagleBone Black (BBB).

> **Read the "Known Issues" section before you start.** Three problems were
> found during code review that affect whether this works out of the box.
> Step 3 ("Reserve DDR memory") below is **required** — without it the
> code can fail to run, or in the worst case destabilize the running Linux
> system.

---

## 0. Will this work on a BeagleBone Black from 2018?

**Hardware: yes.** Every BeagleBone Black revision (including 2018 units,
which are Rev C with the AM335x SoC and 512MB DDR3 RAM) includes the
PRU-ICSS subsystem used by this code. The PRU runs at a fixed 200MHz
regardless of the ARM core's clock speed, so the bit-bang timing in this
project is valid on any BBB revision.

**Software: depends on your OS image.**
- A 2018 BBB most likely shipped with **Debian 9 (Stretch)**. This generally
  has PRU `remoteproc` support (`/sys/class/remoteproc/remoteprocX`), but
  package names for the PRU compiler toolchain (`ti-pru-cgt-v2.3`,
  `pru-software-support-package`) may not exist in that release's repos.
- **Recommended:** re-flash the BBB's eMMC with the latest Debian 12 (or
  current BeagleBoard.org) image before starting. This guarantees PRU
  remoteproc support and access to current PRU toolchain packages, and
  avoids hunting for old package versions.
- After flashing, check your kernel actually has PRU support:
  ```bash
  ls /sys/class/remoteproc/
  uname -r
  ```
  You should see `remoteproc0` and `remoteproc1` (PRU0/PRU1).

---

## 1. Hardware Setup

### 1.1 Wiring (P9 header)

```
BeagleBone Black P9 Header        SPI Device
──────────────────────────        ──────────
P9_31 ──── SCLK  (SPI Clock)  ──── SCK
P9_29 ──── MOSI  (Master Out) ──── MOSI / SDI
P9_30 ──── MISO  (Master In)  ──── MISO / SDO
P9_28 ──── CS0   (active LOW) ──── CS  (device 0)
P9_27 ──── CS1   (active LOW) ──── CS  (device 1, optional)
P9_25 ──── CS2   (active LOW) ──── CS  (device 2, optional)
P9_42 ──── CS3   (active LOW) ──── CS  (device 3, optional)
GND   ──── GND
```

This mapping was verified against the AM335x PRU0 R30/R31 pinout table and
is correct.

### 1.2 Loopback test wiring (optional, recommended first)

For the self-test, connect **P9_29 (MOSI) directly to P9_30 (MISO)** with a
jumper wire. No external SPI device needed.

---

## 2. Software Prerequisites

SSH into the BeagleBone Black and run:

```bash
sudo apt-get update
sudo apt-get install -y \
    ti-pru-cgt-v2.3 \
    pru-software-support-package \
    build-essential
```

If `ti-pru-cgt-v2.3` or `pru-software-support-package` are not found,
search for the correct package names for your Debian release
(`apt-cache search pru`) — naming has changed across Debian/BeagleBoard
image versions.

### 2.1 Disable conflicting pin overlays

The SPI pins used here (P9_28, P9_29, P9_30, P9_31) conflict with the
HDMI/audio cape overlays on some images. Edit `/boot/uEnv.txt`:

```bash
sudo nano /boot/uEnv.txt
```

Add or uncomment:
```
disable_uboot_overlay_video=1
disable_uboot_overlay_audio=1
```

(Don't reboot yet — do step 3 first, then reboot once.)

---

## 3. Reserve DDR Memory for the Shared Buffer (REQUIRED)

**This step is not yet automated by the project and must be done manually.**

The ARM-side code (`pru_spi.c`) maps physical address `0x9F000000` (the top
16MB of a 512MB BBB's RAM) via `/dev/mem` for the TX/RX DMA buffers shared
with the PRU. By default, **Linux owns this entire region** as normal
system RAM. Without reserving it:

- `mmap()` of `/dev/mem` at this address may fail outright if your kernel
  has `CONFIG_STRICT_DEVMEM` enabled (common on modern kernels), **or**
- if it succeeds, the PRU and your program will read/write memory that
  Linux is actively using for processes/kernel data, which can cause
  random crashes or data corruption.

### 3.1 Reserve the memory via the kernel command line

Edit `/boot/uEnv.txt` and locate the line that sets extra kernel
parameters (commonly `cmdline=` or `optargs=`, depending on your image).
Append `mem=496M` to it. For example:

```
optargs=quiet mem=496M
```

This tells Linux to only manage the first 496MB of RAM, leaving the top
16MB (`0x9F000000`–`0x9FFFFFFF`) physically present but untouched by the
kernel — safe for `/dev/mem` access by this driver.

### 3.2 Reboot and verify

```bash
sudo reboot
```

After reboot, confirm the reserved region is excluded from Linux:

```bash
free -m              # should show ~496MB total instead of ~510MB
cat /proc/iomem | grep -i "System RAM"   # top range should stop below 0x9F000000
```

If `CONFIG_STRICT_DEVMEM` is enabled and the region is still reported as
`System RAM`, `/dev/mem` access at `0x9F000000` will be rejected even after
this step — you may need a `reserved-memory` device-tree node instead.
Check with:
```bash
zcat /proc/config.gz 2>/dev/null | grep STRICT_DEVMEM
```

---

## 4. Build

From the project root:

```bash
cd /path/to/spi
make
```

This builds:
- `pru/am335x-pru0-fw` — the PRU0 firmware
- `arm/libpru_spi.a` — the host-side static library
- `arm/pru_spi_example` — the example application

Build PRU or ARM code individually with `make pru` / `make arm`.
`make clean` removes all build artifacts.

---

## 5. Deploy

```bash
sudo ./scripts/deploy.sh
```

This will:
1. Stop the PRU if running
2. Build the PRU firmware and ARM code
3. Install firmware to `/lib/firmware/am335x-pru0-fw`
4. Configure pin muxing (`scripts/setup_pins.sh`)
5. Start PRU0 via remoteproc

To rebuild and redeploy after making changes, just rerun this script.

---

## 6. Run

### 6.1 Loopback self-test (recommended first run)

With P9_29 jumpered to P9_30:

```bash
sudo ./arm/pru_spi_example --loopback
```

Expect `*** LOOPBACK TEST PASSED ***` for transfer sizes 1–1024 bytes.

### 6.2 Full demo (write, full-duplex, read, multi-CS, large transfer)

```bash
sudo ./arm/pru_spi_example
```

### 6.3 Write-only demo

```bash
sudo ./arm/pru_spi_example --write
```

### 6.4 Set SPI mode / speed

```bash
sudo ./arm/pru_spi_example --mode 3 --speed 5000000
```

> See **Known Issues #1** — the `--speed` option currently has no effect
> on the actual SPI clock; the firmware always bit-bangs at the fixed
> ~10 MHz rate regardless of this setting.

---

## 7. Using the API in Your Own Program

```c
#include "pru_spi.h"

int main(void) {
    uint8_t tx[] = {0x9F, 0x00, 0x00};
    uint8_t rx[3];

    if (pru_spi_init() != PRU_SPI_OK) {
        return 1;
    }

    pru_spi_set_mode(0);             /* SPI Mode 0 */
    pru_spi_transfer(0, tx, rx, 3, 0); /* CS0, full-duplex */

    pru_spi_close();
    return 0;
}
```

Build and link against `libpru_spi.a`:

```bash
gcc -I include -I arm -O2 -o myprogram myprogram.c -L arm -lpru_spi -lrt
sudo ./myprogram
```

---

## 8. Known Issues (from code review)

These were found during verification and **have not been fixed** at the
user's request. Be aware of them:

1. **Speed control is non-functional.** `pru_spi_set_speed()` /
   `--speed` computes a `clock_div` value and sends it to the PRU, but
   `pru/pru_spi_fw.c`'s `spi_xfer_byte_modeN()` functions ignore it and
   call `__delay_cycles(4)` (a hardcoded, compile-time constant). The
   actual SPI clock is therefore always fixed at the ~10 MHz rate
   determined by `__delay_cycles(4)`, regardless of what you request.
   If your SPI device requires a slower clock, this driver will currently
   run too fast for it.

2. **`pru/resource_table.h` may fail to build.** It includes
   `<pru_types.h>`, but `struct resource_table` is normally provided by
   `<rsc_types.h>` in the TI pru-software-support-package. If `make pru`
   fails with an undefined-type error on `resource_table`, change the
   include in `pru/resource_table.h` from `<pru_types.h>` to
   `<rsc_types.h>`.

3. **DDR buffer region not reserved automatically.** See Section 3 —
   you must manually reserve `0x9F000000`–`0x9FFFFFFF` via `mem=496M` (or
   a device-tree `reserved-memory` node) before running anything that
   calls `pru_spi_init()`.

---

## 9. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `cannot open /dev/mem` | Run as root: `sudo ./arm/pru_spi_example` |
| `mmap PRUSS failed` or `mmap DDR buffer ... failed` | DDR memory not reserved (Section 3) or `CONFIG_STRICT_DEVMEM` blocking access |
| `failed to start PRU` | Check `ls /sys/class/remoteproc/`, `ls -la /lib/firmware/am335x-pru0-fw`, `dmesg \| tail -20` |
| Timeout waiting for PRU readiness | PRU firmware crashed or never started — check `dmesg`, verify `pru/resource_table.h` builds correctly (Known Issue #2) |
| Loopback test fails | Check P9_29↔P9_30 jumper wiring; verify pin mux with `config-pin -q P9.29` / `config-pin -q P9.30` |
| PRU firmware doesn't respond | `cat /sys/class/remoteproc/remoteproc1/state` should be `running`; try `remoteproc0`/`remoteproc2` if not |
