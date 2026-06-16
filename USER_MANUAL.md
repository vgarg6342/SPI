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

### 6.4 Set SPI mode

```bash
sudo ./arm/pru_spi_example --mode 3
```

> The SPI **clock speed is set at compile time** via `SPI_SCLK_HZ` in
> `include/pru_spi_common.h` (the PRU `__delay_cycles()` intrinsic requires a
> constant). To change it, edit that define and rebuild. The `--speed` flag and
> `pru_spi_set_speed()` are no-ops kept only for source compatibility.

### 6.5 Parallel 4-DAC write

```bash
sudo ./arm/pru_spi_example --parallel
```

Drives 4 DACs at once, all from the PRU: shared SCLK on **P9_31**, shared
CS/SYNC on **P8_11** (active LOW), and 4 independent data lanes **P9_29 / P9_28
/ P9_27 / P9_25** (MOSI0..3 → DAC0..3). Each DAC receives different data, and the
shared CS latches all 4 together every frame. Wire a logic analyzer on SCLK +
CS + the 4 lanes to watch them shift out and latch.

### 6.6 Continuous 4-DAC streaming (10k samples/s, the main use case)

This is the recommended path for driving 4 DACs at a sustained sample rate (e.g.
**10,000 samples/s**) from a data file. It uses a **ring buffer in PRU shared
RAM — no DDR** — and the PRU paces every sample off its **IEP timer**, so the
sample spacing is exact and glitch-free even while Linux is busy.

```bash
# 1) Generate a 4-column sine table on the dev PC (no board needed):
python3 scripts/gen_adc.py --samples 200 --periods 1 --out adc.txt

# 2) On the BeagleBone, after make + deploy:
sudo ./arm/dac_stream --file adc.txt --rate 10000 --rt
```

**`adc.txt` format.** One sample per line, four integers `DAC0 DAC1 DAC2 DAC3`,
each a 12-bit code `0..4095`. Whitespace or commas separate columns; blank lines
and `#` comments are ignored. Out-of-range values are clamped to 4095 with a
warning.

**Wiring** (same pins as parallel mode): shared SCLK **P9_31**, shared CS/SYNC
**P8_11** (active LOW), data lanes **P9_29 / P9_28 / P9_27 / P9_25** =
MOSI0..3 → DAC0..3. Tie each DAC's `LDAC` low (or to the shared CS) so it latches
with the frame.

**DAC framing.** Each 12-bit code is wrapped into a 16-bit word by `DAC_FRAME()`
in `include/pru_spi_common.h` — default `0x3000 | (value & 0x0FFF)` for the
**MCP4921/4922** (DAC_A, 1× gain, output active). **Edit `DAC_CTRL_BITS` for your
exact DAC's command/control bits.**

**`dac_stream` options:** `--file F`, `--rate N` (samples/s, default 10000),
`--count N` (play only the first N samples), `--rt` (request SCHED_FIFO). On
completion it prints the samples queued and the **underflow count** (should be 0).

**API** (see `arm/pru_spi.h`) if you want to feed samples from your own program:

```c
pru_spi_init();
pru_dac_stream_start(10000);              /* 10 kHz */
uint16_t s[4] = { d0, d1, d2, d3 };       /* raw 12-bit codes */
pru_dac_stream_push(s);                    /* blocks if ring full; never drops */
int underflows = pru_dac_stream_end(0);    /* drains + stops, returns underflows */
pru_spi_close();
```

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

1. **Speed is set at compile time (was: non-functional runtime control).**
   The old runtime `clock_div` plumbing never worked — `__delay_cycles()` is a
   compiler intrinsic and requires a constant. This has been resolved by
   deriving the bit-bang delay from a single `#define`, `SPI_SCLK_HZ`, in
   `include/pru_spi_common.h` (see `SCLK_DELAY_CYCLES`). To change the SPI
   clock: edit `SPI_SCLK_HZ`, `make`, and redeploy the firmware.

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

## 8a. Parallel 4-DAC mode — things to check / likely gotchas

This mode (`pru_spi_parallel_write()`, firmware `CMD_TRANSFER_PARALLEL`) has
**not been compiled or hardware-tested** on this machine. Probable issues to
look at when you bring it up on the board:

1. **Shared CS/SYNC is driven by the PRU (P8_11, R30 bit 15, active LOW).**
   All 4 DACs share this one line, so the PRU frames every word with it and all
   4 DACs latch together on the CS-rising edge. Consequence: you cannot address
   a single DAC on its own — **every frame updates all 4 at once**. If your DAC
   needs a separate LDAC pulse, tie LDAC low or to the shared CS per its
   datasheet. Check on a scope that CS goes low for exactly `SPI_FRAME_BITS`
   clocks per frame and rises to latch. Tune `PAR_CS_SETUP/HOLD/GAP_CYCLES` in
   `include/pru_spi_common.h` if your DAC needs more setup/hold around CS.

2. **Pin overlap between the two modes.** R30 bits 3/5/7 are CS0/CS1/CS2 in
   single-MOSI mode but MOSI1/MOSI2/MOSI3 in parallel mode (same physical pins
   P9_28/P9_27/P9_25); the parallel shared CS on P8_11 is parallel-mode only.
   Don't expect one wiring to serve both roles at once — wire for the mode
   you're using.

3. **Verify the actual SCLK frequency on a scope.** `SCLK_DELAY_CYCLES` is
   derived from `SPI_SCLK_HZ` minus a rough loop-overhead estimate
   (`SCLK_LOOP_OVERHEAD`, ~2 cycles); the R30 stores also cost cycles, so the
   real clock will be somewhat below `SPI_SCLK_HZ`. Tune `SPI_SCLK_HZ` (and, if
   needed, `SCLK_LOOP_OVERHEAD`) until the scope reads what your DAC needs.
   Very high `SPI_SCLK_HZ` clamps the delay to a 1-cycle minimum.

4. **SPI mode and frame width must match your DAC.** Defaults are mode 0 and
   `SPI_FRAME_BITS = 16`. Set `pru_spi_set_mode()` and `SPI_FRAME_BITS` for your
   part. Data is shifted **MSB-first**; if your DAC expects a different bit
   order or a command/address nibble in the upper bits, pack that into the
   `uint16_t` values you pass in.

5. **Frame width > 16 bits needs an API change.** The transpose reads
   `uint16_t` data. If your DAC frame is 24/32-bit, widen the buffer type to
   `uint32_t` in `pru_spi_parallel_write()` (and `SPI_FRAME_BITS`) accordingly.

6. **DDR stream size.** The pre-transposed stream is `num_frames * frame_bits`
   32-bit words in the DDR TX buffer (64 KB by default) — i.e. up to ~1024
   frames of 16-bit data. For more, enlarge the DDR buffer (Section 3 / 
   `DDR_BUF_DEFAULT_SIZE`).

7. **Selecting fewer than 4 DACs.** `pru_spi_set_num_dacs(n)` (1..4, runtime)
   limits how many lanes carry data; lanes `n`..3 are held LOW (idle) because
   the ARM-side transpose zeroes their bits and the PRU drives those pins low.
   Buffers for inactive lanes are ignored (pass NULL). SCLK and the shared CS
   still run normally — the idle lanes just output 0. Verify on a scope that
   only the lanes you selected toggle. Default is all 4.

---

## 8c. Continuous streaming mode — things to check / likely gotchas

The streaming path (`pru_dac_stream_*()`, firmware `CMD_STREAM_START`,
`arm/dac_stream`) has **not been compiled or hardware-tested** on this machine.
Bring-up checklist:

1. **SPI is now MODE 0 ONLY at 1 MHz.** `SPI_SCLK_HZ` was changed to `1000000`
   and SPI modes 1/2/3 were removed from the firmware. `pru_spi_set_mode()`
   rejects anything but 0. Verify SCLK ≈ 1 MHz on a scope (it runs slightly below
   `SPI_SCLK_HZ` due to per-bit store overhead; tune `SPI_SCLK_HZ` /
   `SCLK_LOOP_OVERHEAD` if needed).

2. **IEP timer pacing — verify on-device.** The PRU paces each sample with the
   IEP free-running counter (`pru_iep.h`, `CT_IEP.TMR_CNT`, `DEFAULT_INC=1`).
   This assumes the IEP advances one tick per 5 ns (200 MHz). Confirm the
   frame-to-frame period on a scope is **100 µs at 10 kHz** (`--rate`). If the
   period is off by a fixed factor, the IEP increment/clock source differs on
   your image — adjust `iep_timer_init()` / `sample_period_cycles` accordingly.
   The `pru_iep.h` header ships with the pru-software-support-package.

3. **Sample rate is runtime; SPI clock is compile-time.** Unlike `SPI_SCLK_HZ`,
   the sample rate is carried in the shared-RAM control block
   (`sample_period_cycles`), so `--rate` works without rebuilding. Ensure your
   chosen rate leaves room for one 16-bit frame: at 1 MHz a frame is ~16 µs, so
   rates up to ~50 kHz are safe; the firmware/period must satisfy
   `sample_period_cycles > frame cycles` or frames will run back-to-back.

4. **Ring depth and underflow.** The ring holds `RING_CAPACITY_FRAMES` (1024)
   frames ≈ 102 ms at 10 kHz, all in shared RAM (8 KB of the 12 KB). If the ARM
   producer can't keep up, the PRU **holds the last sample** (no wild glitch),
   counts it in `underflow_count`, and keeps the cadence. `dac_stream` prints the
   count — expect **0**. Use `--rt` and keep the box otherwise idle for long
   runs; `mlockall()` is always applied.

5. **DAC control bits.** `DAC_CTRL_BITS = 0x3000` targets MCP49xx (DAC_A, 1×
   gain, active). For a different DAC, or to use gain 2× / channel B / buffered,
   edit `DAC_CTRL_BITS` or `DAC_FRAME()` in `include/pru_spi_common.h`. Data is
   MSB-first, 16 bits, shared-CS framed (all 4 DACs latch together).

6. **Shared-RAM layout must match between ARM and PRU.** The command block
   (0x00), stream control block (0x40), and ring data (0x80) live in PRU shared
   RAM; the compile-time checks `PRU_DAC_STREAM_SIZE_CHECK` and
   `PRU_RING_FIT_CHECK` guard the sizes. If you change `RING_CAPACITY_FRAMES`,
   keep it a power of two so the head/tail mask arithmetic stays correct.

---

## 8b. Sync-to-BeagleBone workflow — things to check

Builds happen on the BeagleBone, not on the editing machine. Use
`scripts/sync_to_bbb.sh` (or `make sync`) from a PC that can reach the board.
Likely gotchas:

1. **Executable bit / how to run.** If the script lost its `+x` bit (e.g. after
   a git clone), run it as `bash scripts/sync_to_bbb.sh`.

2. **`rsync` and `ssh` must be installed** on both the intermediate PC and the
   BeagleBone (BBB images usually have both; `sudo apt-get install rsync` if
   not).

3. **Host/credentials.** Defaults are `debian@192.168.7.2` (USB networking) and
   destination `/home/debian/DERIC_testing`. Override with
   `BBB_HOST=... BBB_USER=... BBB_DEST=... ./scripts/sync_to_bbb.sh`. Set up SSH
   keys or be ready to type the password.

4. **`--delete` is destructive on the target.** The rsync uses `--delete`, so
   anything in `/home/debian/DERIC_testing` that isn't in your source tree is
   removed. Don't keep unsynced work only on the board there.

5. **Then build on the board:** `ssh` in, `cd /home/debian/DERIC_testing`,
   `make`, `sudo ./scripts/deploy.sh`. Or use `./scripts/sync_to_bbb.sh --deploy`
   to do it over SSH in one shot.

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
