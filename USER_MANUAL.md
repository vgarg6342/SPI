# User Manual — PRU Single-DAC Player (BeagleBone Black)

This manual is the complete guide to setting up, building, deploying, running,
configuring, and troubleshooting the PRU0 single-DAC player on a BeagleBone
Black (BBB, AM335x).

It does **one** thing: read a table of 12-bit DAC codes, frame each into a
16-bit SPI word (MCP4921-style), put them in DDR, and have the **PRU copy them
out of DDR and shift them to one SPI DAC** on SPI mode 0 at a fixed sample rate.

> ⚠️ **Build location.** Your editing PC is edit-only. The TI PRU compiler
> (`clpru`) and the native ARM `gcc` live on the BeagleBone. Always sync the
> sources to the board and build there (§4, §8b).

**Contents**
1. [How it works](#1-how-it-works)
2. [Hardware & wiring](#2-hardware--wiring)
3. [Board preparation: `uEnv.txt`, pins, DDR reservation](#3-board-preparation-uenvtxt-pins-ddr-reservation)
4. [Build & deploy](#4-build--deploy)
5. [Configuration reference (every macro)](#5-configuration-reference-every-macro)
6. [Detecting a crashed / hung PRU](#6-detecting-a-crashed--hung-pru)
7. [Running: `adc.txt`, `dac_load`, the ARM API](#7-running-adctxt-dac_load-the-arm-api)
8. [Troubleshooting](#8-troubleshooting)
   - [8a. DDR & timing caveats (verify on hardware)](#8a-ddr--timing-caveats-verify-on-hardware)
   - [8b. Sync / build caveats](#8b-sync--build-caveats)
9. [Verifying on a scope / logic analyzer](#9-verifying-on-a-scope--logic-analyzer)

---

## 1. How it works

```
  ARM (Linux userspace)                         PRU0 (firmware)
  ─────────────────────                         ───────────────
  load adc.txt (12-bit codes)
        │
  DAC_FRAME(): code -> 16-bit word
        │  (apply the 4 control bits)
        ▼
  write words to DDR buffer  ──────────────▶  copy DDR -> local SRAM (chunks)
  set cmd: addr,count,period                        │
  command = CMD_PLAY        ──────────────▶   for each sample:
        │                                          shift 16 bits, MSB first
  poll status + heartbeat   ◀──────────────       CS low → bits → CS high (latch)
        │                                          __delay_cycles(period - frame)
        ▼                                          heartbeat++  ── liveness
  return when STATUS_DONE                     status = STATUS_DONE
  (or PRU_DAC_ERR_HUNG if heartbeat freezes)
```

- **One DAC, SPI mode 0** (CPOL=0/CPHA=0: clock idle low, DAC samples on the
  rising edge). Bit clock is a compile-time constant (default 1 MHz).
- **The PRU does the DDR copy.** It reads the samples from a DDR physical
  address over its OCP master port into its own 8 KB data RAM, 512 samples at a
  time, and plays from there.
- **The sample rate is generated with `__delay_cycles()`** — there is **no IEP
  timer** (`pru_iep.h` is not available on the target). Because `__delay_cycles()`
  needs a compile-time constant, the rate is fixed at build time
  (`DAC_SAMPLE_RATE_HZ`). The firmware delays `period − frame-shift-time` after
  each word so the real rate stays close to the configured value (§5.3, §8a).
- **The 16-bit word = 4 control bits + 12 data bits.** `DAC_FRAME(v)` =
  `DAC_CTRL_BITS | (v & 0x0FFF)`. Default `DAC_CTRL_BITS = 0x3000` selects
  MCP49xx DAC_A, unbuffered, 1× gain, output active.

---

## 2. Hardware & wiring

Three PRU0 outputs drive one SPI DAC. No MISO (DACs are write-only).

| Signal | PRU0 R30 bit | Header pin | Connect to DAC |
|--------|--------------|------------|----------------|
| SCLK   | bit 0 | **P9_31** | SCK |
| SDI    | bit 1 | **P9_29** | SDI / DIN / MOSI |
| CS     | bit 3 | **P9_28** | CS / SYNC (active LOW) |
| GND    | —     | **P9_1 / P9_2** | VSS / AGND |
| 3V3    | —     | **P9_3 / P9_4** | VDD (if the DAC runs at 3.3 V) |

- Tie **LDAC** LOW (or to CS) so the DAC latches its output when CS rises.
- Logic is **3.3 V**. Do **not** drive a 5 V-only DAC input directly without a
  level shifter, and never feed >3.3 V back into a BBB pin.
- Keep wires short; at 1 MHz this is forgiving, but a common ground is essential.

> These three pins (P9_31/P9_29/P9_28) are the standard PRU0 `pr1_pru0_pru_r30_0/1/3`
> outputs. If you change them, update both `include/pru_spi_common.h` (the `*_BIT`
> macros) **and** `scripts/setup_pins.sh`.

---

## 3. Board preparation: `uEnv.txt`, pins, DDR reservation

Do this **once** on the BeagleBone, then reboot. All three pieces below matter.

### 3.1 Edit `/boot/uEnv.txt`

Open it on the board:

```bash
sudo nano /boot/uEnv.txt
```

**(a) Reserve DDR for the sample buffer.** The ARM maps a physically-contiguous
DDR region via `/dev/mem`; Linux must not also be using it. The supported way on
the BBB is to shrink the kernel's view of RAM so the top of DDR is free.

The BBB has **512 MB** of DDR at `0x80000000 … 0x9FFFFFFF`. Telling the kernel to
use only 496 MB leaves the **top 16 MB free**:

```
  mem=496M  →  kernel RAM = 0x80000000 … 0x9EFFFFFF
               FREE region = 0x9F000000 … 0x9FFFFFFF  (16 MB)
```

`0x9F000000` is exactly `DDR_BUF_PHYS_BASE` in `include/pru_spi_common.h`.

Find the kernel command-line line in `uEnv.txt` (it is usually `cmdline=…`, or on
some images `optargs=…`) and append `mem=496M`. For example:

```text
cmdline=coherent_pool=1M net.ifnames=0 rng_core.default_quality=100 quiet mem=496M
```

If there is no such line, add:

```text
optargs=mem=496M
```

> **Match the number to your board.** 512 MB board → `mem=496M` (16 MB free).
> Need more than 16 MB (long durations)? Use `mem=480M` (32 MB free) and set
> `DDR_BUF_PHYS_BASE = 0x9E000000`. The free region must be ≥ `DDR_BUF_BYTES`
> (= `STREAM_MAX_SAMPLES × 2`). At 10 kHz that's 20 KB per second, so 16 MB ≈ 800 s.

**(b) Free the header pins.** The HDMI/audio overlays claim several P8/P9 pins.
Disable the ones you don't need so P9_31/P9_29/P9_28 are available, and keep the
universal cape (it provides `config-pin`):

```text
disable_uboot_overlay_video=1
disable_uboot_overlay_audio=1
enable_uboot_cape_universal=1
```

**(c) Make sure the PRU remoteproc is enabled.** On 4.14+/4.19+ TI kernels the
PRU-RPROC overlay is normally on by default. If `/sys/class/remoteproc/` has no
PRU node after boot, explicitly enable it (filename depends on your kernel —
list `/lib/firmware/*PRU-RPROC*` to find yours):

```text
uboot_overlay_pru=/lib/firmware/AM335X-PRU-RPROC-4-19-TI-00A0.dtbo
```

Save, then **reboot**:

```bash
sudo reboot
```

### 3.2 Verify after reboot

```bash
cat /proc/cmdline                     # should contain mem=496M
dmesg | grep -i memory                # kernel reports ~496M usable
grep "System RAM" /proc/iomem         # top of RAM should end at 9effffff
ls /sys/class/remoteproc/             # expect remoteproc1 (PRU0); maybe more
```

If `/proc/iomem` shows System RAM ending at `9effffff`, the region
`0x9f000000…0x9fffffff` is yours.

> **Which remoteproc is PRU0?** This project assumes
> `/sys/class/remoteproc/remoteproc1`. On some kernels the numbering differs.
> Check with:
> ```bash
> for d in /sys/class/remoteproc/remoteproc*; do echo "$d: $(cat $d/name)"; done
> ```
> If PRU0 is not `remoteproc1`, update `REMOTEPROC_PRU0_PATH` in
> `include/pru_spi_common.h` (and `REMOTEPROC_PATH` in `scripts/deploy.sh`).

### 3.3 Pin mux

`scripts/deploy.sh` runs `scripts/setup_pins.sh` for you, but you can run it
directly:

```bash
sudo ./scripts/setup_pins.sh
```

It muxes P9_31/P9_29/P9_28 to `pruout` and prints each pin's state.

---

## 4. Build & deploy

### 4.1 Push sources from the PC (you cannot build on the PC)

```bash
# defaults: debian@192.168.7.2 → /home/debian/DERIC_testing
bash ./scripts/sync_to_bbb.sh
# or override the host:
BBB_HOST=beaglebone.local bash ./scripts/sync_to_bbb.sh
# sync + build + deploy in one shot (build/deploy happen on the board):
bash ./scripts/sync_to_bbb.sh --deploy
```

### 4.2 Build + deploy on the board

```bash
ssh debian@192.168.7.2
cd /home/debian/DERIC_testing

make                       # builds PRU firmware (clpru) + ARM library/example (gcc)
sudo ./scripts/deploy.sh   # installs firmware, muxes pins, starts PRU0
```

`deploy.sh` flags: `--build-only` (compile, don't start), `--no-build` (start the
already-built firmware). It copies `pru/am335x-pru0-fw` to `/lib/firmware/` and
writes `start` to the remoteproc `state`.

Build targets (top-level `make`): `make pru`, `make arm`, `make clean`.

> If `make` can't find `clpru` / headers, set the tool paths (see §8b):
> `make -C pru PRU_CGT=/usr/share/ti/cgt-pru PRU_SWPKG=/usr/lib/ti/pru-software-support-package`

---

## 5. Configuration reference (every macro)

Everything is in **`include/pru_spi_common.h`** — the single source of truth
shared by both the PRU and ARM builds. After changing any of these, **rebuild
and redeploy the firmware** (the SPI clock and CS framing are baked in at compile
time because `__delay_cycles()` requires constant arguments).

### 5.1 SPI bit clock

| Macro | Default | Notes |
|---|---|---|
| `SPI_SCLK_HZ` | `1000000` (1 MHz) | The SPI bit clock. Derived `SCLK_DELAY_CYCLES` feeds `__delay_cycles()`. |
| `SCLK_LOOP_OVERHEAD` | `3` | Cycles the bit body costs; tune if your measured clock is off (§8a). |

PRU runs at 200 MHz → half-period at 1 MHz is 100 cycles. To set, e.g., 2 MHz:
set `SPI_SCLK_HZ` to `2000000`, rebuild. Don't exceed your DAC's max SCK.

### 5.2 DAC framing (the 4 control bits)

| Macro | Default | Notes |
|---|---|---|
| `DAC_BITS` | `16` | SPI word width. |
| `DAC_VALUE_MAX` | `0x0FFF` | 12-bit full scale (4095). |
| `DAC_CTRL_BITS` | `0x3000` | OR'd into every code. MCP49xx: bit15 A/B, bit14 BUF, bit13 GA, bit12 SHDN. `0x3000` = DAC_A, unbuffered, 1× gain, active. |
| `DAC_FRAME(v)` | — | `DAC_CTRL_BITS \| (v & 0x0FFF)`. |

For a different DAC, set `DAC_CTRL_BITS` to that part's command/config bits. If
your DAC isn't "4 control + 12 data", change `DAC_FRAME()` accordingly.

### 5.3 Sample rate & transmission duration (the "control variables")

The sample rate is **fixed at compile time** — the PRU paces with
`__delay_cycles()`, which requires a constant, so there is no runtime rate.

| Macro | Default | Notes |
|---|---|---|
| `DAC_SAMPLE_RATE_HZ` | `10000` (10k S/s) | The sample rate. To change it, edit this and rebuild the firmware. |
| `DAC_SAMPLE_PERIOD_CYCLES` | `PRU_CLK_HZ/rate` (20000) | Target sample period in PRU cycles = 1/rate. |
| `FRAME_SHIFT_CYCLES` | derived | Approx cycles spent shifting one 16-bit word; subtracted so the period stays accurate (tune via `SCLK_LOOP_OVERHEAD`, §8a). |
| `DAC_SAMPLE_DELAY_CYCLES` | `period − frame` | The value actually fed to `__delay_cycles()` after each word. For the dead-simple "delay = whole period" behaviour, use `DAC_SAMPLE_PERIOD_CYCLES` in the firmware instead. |
| `STREAM_DURATION_SEC` | `5` | Transmission length. **Sizes the DDR buffer** and **caps a run**. |
| `STREAM_MAX_SAMPLES` | `rate × duration` | DDR capacity in samples; also the max `num_samples` per play. |

Want a longer run? Raise `STREAM_DURATION_SEC` and rebuild (and confirm the
reserved DDR region in §3.1 is still big enough). Want a different rate? Edit
`DAC_SAMPLE_RATE_HZ` and rebuild the firmware.

### 5.4 Memory / DDR

| Macro | Default | Notes |
|---|---|---|
| `DDR_BUF_PHYS_BASE` | `0x9F000000` | Physical address of the DDR buffer. **Must be reserved** (§3.1). |
| `DDR_BUF_BYTES` | `STREAM_MAX_SAMPLES × 2` | Buffer size the ARM mmaps. |
| `SRAM_CHUNK_SAMPLES` | `512` | Samples copied DDR→local per chunk (1 KB; fits PRU 8 KB DRAM). |
| `PRUSS_BASE_ADDR`, `PRU_SHAREDMEM_OFFSET` | AM335x | Don't change unless porting. |

### 5.5 ARM-side knob

| Symbol | Where | Default | Notes |
|---|---|---|---|
| `WATCHDOG_FLOOR_MS` | `arm/pru_spi.c` | `300` | Minimum heartbeat-stall time before `pru_dac_play()` declares the PRU hung. The effective watchdog is `max(floor, ~5 sample periods)`, so low sample rates don't false-trigger. |

---

## 6. Detecting a crashed / hung PRU

**The problem this solves:** previously, if the firmware crashed mid-run, the
ARM side polled `status == BUSY` forever — "the PRU is shown busy" with no way to
know it was dead.

**The mechanism:** the firmware increments `cmd->heartbeat` on **every sample**
during playback, and on **every iteration** of its idle loop. A running PRU
therefore always has a moving heartbeat.

**What `pru_dac_play()` does:** while waiting for `STATUS_DONE`, it watches the
heartbeat. If it stops advancing for longer than the watchdog window
(`WATCHDOG_FLOOR_MS` = 300 ms, or ~5 sample periods if that is larger) while the
status is not `DONE`, it:
- stops waiting and returns **`PRU_DAC_ERR_HUNG`**,
- prints the **remoteproc `state`** (e.g. `crashed`, `offline`, `running`) and
  how many samples were done,
- points you at `dmesg`.

**Check liveness on demand:**

```bash
sudo ./arm/dac_load --status
# → "remoteproc state 'running', PRU ALIVE"   (good)
# → "remoteproc state 'crashed', PRU HUNG/DEAD"
```

or in code:

```c
if (pru_dac_is_alive() != 1) { /* heartbeat frozen → crashed */ }
char st[32]; pru_dac_get_state(st, sizeof st);   /* remoteproc state string */
```

**When you see a hang:**

```bash
dmesg | tail -30        # remoteproc usually logs a PRU fault/crash here
cat /sys/class/remoteproc/remoteproc1/state
```

**Recover** (reload the firmware):

```bash
sudo ./scripts/deploy.sh --no-build      # stop → start PRU0 with the fw
# or manually:
echo stop  | sudo tee /sys/class/remoteproc/remoteproc1/state
echo start | sudo tee /sys/class/remoteproc/remoteproc1/state
```

**Common causes of a PRU hang** (the PRU has no MMU, so it doesn't "segfault" —
it bus-errors or wedges):
- The DDR address isn't actually reserved/accessible → the OCP read stalls.
  Re-check §3.1 / §3.2 (`/proc/cmdline`, `/proc/iomem`).
- The OCP master port wasn't enabled (it is, in `main()`:
  `CT_CFG.SYSCFG_bit.STANDBY_INIT = 0`) — don't remove that line.
- A firmware edit introduced an out-of-range write or an infinite loop.

---

## 7. Running: `adc.txt`, `dac_load`, the ARM API

### 7.1 `adc.txt` format

One **12-bit code (0…4095) per line**. Blank lines and lines starting with `#`
are ignored. Out-of-range values are clamped (with a warning).

```text
# my waveform — 12-bit codes, one per line
2048
2148
2248
...
```

### 7.2 Generate a test file

`scripts/gen_adc.py` runs on the PC (or the board):

```bash
python3 scripts/gen_adc.py                                  # 5 s, 10kS/s, 1kHz sine
python3 scripts/gen_adc.py --rate 10000 --duration 5 --freq 100
python3 scripts/gen_adc.py --waveform square --freq 50 --out sq.txt
python3 scripts/gen_adc.py --waveform tri --amplitude 4000 --offset 2048
```

Options: `--rate`, `--duration`, `--freq`, `--waveform {sine,ramp,square,tri}`,
`--amplitude`, `--offset`, `--out`.

> The waveform is played back at the firmware's **fixed** `DAC_SAMPLE_RATE_HZ`.
> Set `gen_adc.py --rate` to the **same** value so the waveform frequency comes
> out as intended (it only affects how the file is generated, not playback).

### 7.3 The example: `dac_load`

```bash
sudo ./arm/dac_load                              # adc.txt, default duration
sudo ./arm/dac_load --file wave.txt
sudo ./arm/dac_load --duration 5
sudo ./arm/dac_load --status                     # report PRU liveness and exit
sudo ./arm/dac_load --help
```

Samples played = `min(lines in file, DAC_SAMPLE_RATE_HZ × duration, DDR
capacity)`. The sample rate itself is fixed at compile time. The run blocks until
the PRU finishes (or reports a crash) and prints the result.

### 7.4 The ARM API ([arm/pru_spi.h](arm/pru_spi.h))

```c
#include "pru_spi.h"
#include "pru_spi_common.h"

int main(void) {
    if (pru_dac_init() != PRU_DAC_OK) return 1;

    uint16_t codes[50000];
    for (int i = 0; i < 50000; i++) codes[i] = 2048;   /* 12-bit codes */

    int r = pru_dac_play(codes, 50000, 0);             /* blocks; rate is fixed */
    if (r < 0) fprintf(stderr, "%s\n", pru_dac_strerror(r));

    pru_dac_close();
    return 0;
}
```

| Function | Purpose |
|---|---|
| `pru_dac_init()` | Map memory, load + start PRU0, wait for the readiness magic. |
| `pru_dac_play(codes, n, timeout_ms)` | Frame `codes` into DDR, run, watch the heartbeat. Blocks. Sample rate is the compile-time `DAC_SAMPLE_RATE_HZ`. `timeout_ms=0` → derived from run length + 1 s. Returns `n`, or a negative `PRU_DAC_ERR_*`. |
| `pru_dac_is_alive()` | 1 = heartbeat moving, 0 = frozen. |
| `pru_dac_get_state(buf,n)` | Copy the remoteproc state string. |
| `pru_dac_capacity_samples()` | `STREAM_MAX_SAMPLES`. |
| `pru_dac_strerror(err)` | Message for an error code. |
| `pru_dac_close()` | Halt PRU, unmap. Idempotent. |

Return codes: `PRU_DAC_OK`, `…_NOT_INIT`, `…_MMAP`, `…_FIRMWARE`, `…_PARAM`,
`…_BUSY`, `…_TIMEOUT`, `…_PRU`, `…_HUNG`, `…_TOO_LARGE`.

Build against the library: `gcc myapp.c -I include -I arm -L arm -lpru_spi -lrt`.

---

## 8. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `cannot open /dev/mem` / mmap failed | Not root. Run with `sudo`. |
| `mmap DDR buffer at 0x9F000000 … failed` | DDR not reserved. Do §3.1 (`mem=496M`) and reboot; verify with `/proc/iomem`. |
| `PRU not ready (magic=0x00000000)` | Firmware didn't start. `cat /sys/class/remoteproc/remoteproc1/state`; `dmesg | tail`. Wrong remoteproc number? See §3.2. |
| `Firmware load/start failed` | `am335x-pru0-fw` not in `/lib/firmware/`. Re-run `sudo ./scripts/deploy.sh`. |
| **PRU HUNG (heartbeat frozen)** | Firmware crashed. See §6: `dmesg`, reload with `deploy.sh --no-build`. |
| Nothing on the scope | Pins not muxed (`sudo ./scripts/setup_pins.sh`), HDMI/audio overlay still claiming pins (§3.1b), wrong wiring, or LDAC not tied low. |
| Output updates but value looks wrong | `DAC_CTRL_BITS` doesn't match your DAC; or your DAC expects a different bit order/word width — adjust `DAC_FRAME()`/`DAC_BITS`. |
| Measured SPI clock ≠ `SPI_SCLK_HZ`, or sample rate slightly off | Tune `SCLK_LOOP_OVERHEAD` and rebuild (§8a). |
| Sample rate far below `DAC_SAMPLE_RATE_HZ` | The word no longer fits the period (`DAC_SAMPLE_DELAY_CYCLES` clamped to 1). Lower the rate or raise `SPI_SCLK_HZ` (§8a). |
| `more samples than the DDR buffer holds` | Raise `STREAM_DURATION_SEC` (and the reserved region) and rebuild. |
| `config-pin: not found` | `sudo apt-get install bb-cape-overlays`; ensure `enable_uboot_cape_universal=1`. |

### 8a. DDR & timing caveats (verify on hardware)

These cannot be checked on the edit-only PC — confirm them on the board with a
scope/logic analyzer:

- **SPI clock accuracy.** `SCLK_DELAY_CYCLES = SCLK_HALF_CYCLES − SCLK_LOOP_OVERHEAD`.
  The `−SCLK_LOOP_OVERHEAD` compensates for the few instructions in the bit loop
  (set SDI, branch). If your measured SCK is slightly fast/slow, tune
  `SCLK_LOOP_OVERHEAD` and rebuild. At 1 MHz the error from a 1-cycle (5 ns)
  miscount is ~0.5%.
- **Sample-rate accuracy (no IEP).** The rate is made with `__delay_cycles()`,
  not a free-running timer, so it is only as accurate as the cycle accounting.
  The firmware delays `DAC_SAMPLE_DELAY_CYCLES = DAC_SAMPLE_PERIOD_CYCLES −
  FRAME_SHIFT_CYCLES`, where `FRAME_SHIFT_CYCLES` is an **estimate** of the time
  to shift one word. If the measured sample rate is a few % off, the estimate is
  slightly wrong — adjust `SCLK_LOOP_OVERHEAD` (it feeds both the bit timing and
  `FRAME_SHIFT_CYCLES`) and rebuild, or measure the real per-word time on a scope
  and refine `FRAME_SHIFT_CYCLES`. Small per-sample bookkeeping (counter stores,
  the STOP check, the per-chunk DDR copy) is not subtracted; at 10 kHz it is
  negligible, but it grows in relative terms as you push the rate up.
- **Max sample rate.** Each 16-bit word takes ~`(16 × 2 × half-period) + CS
  setup/hold`. At 1 MHz that's ~16 µs + framing, so the sample **period** must be
  longer than that. 10 kHz (100 µs) leaves a wide margin. If you push the rate up
  (or the bit clock down) until the word no longer fits the period,
  `DAC_SAMPLE_DELAY_CYCLES` clamps to 1 and the actual rate is limited by the word
  time, not your macro — the output rate will be lower than requested.
- **DDR copy timing.** The PRU copies one 512-sample chunk (1 KB) from DDR
  between samples; that read is microseconds and hides easily in the ~84 µs of
  slack per sample at the defaults. At very high sample rates the chunk copy adds
  a one-off stretch to the sample at each chunk boundary — reduce
  `SRAM_CHUNK_SAMPLES` to spread it more evenly if that ever matters.
- **DDR region must be reserved AND large enough.** `mem=` must leave a hole ≥
  `DDR_BUF_BYTES` starting exactly at `DDR_BUF_PHYS_BASE`. Mismatch → mmap fails
  or (worse) you read RAM Linux is using → garbage or a PRU stall.
- **Cache/coherency.** `/dev/mem` here is mapped uncached (`O_SYNC`) and the ARM
  side issues `__sync_synchronize()` before kicking the PRU, so the PRU sees the
  samples. Keep that barrier if you modify `pru_dac_play()`.

### 8b. Sync / build caveats

- **Build on the board, not the PC.** `clpru` and the
  pru-software-support-package are on the BeagleBone. Editor errors on the PC
  (`pru_spi_common.h not found`, unknown `__R30` / `__delay_cycles`) are expected
  and disappear under the on-device toolchain.
- **`sync_to_bbb.sh` uses `rsync --delete`** to `/home/debian/DERIC_testing`. Make
  sure `BBB_DEST` is correct so you don't wipe the wrong directory. Override host
  with `BBB_HOST=…`; it excludes `.git/`, build artifacts, and `arm/dac_load`.
- **PRU tool paths.** If `make -C pru` fails to find the compiler/headers, pass
  `PRU_CGT=` and `PRU_SWPKG=` (see §4.2) or edit `pru/Makefile`.
- **Linker memory map.** `pru/AM335x_PRU.cmd` reflects the AM335x PRU memory map
  (8 KB IMEM, 8 KB DMEM, 12 KB shared). Don't change it unless you understand the
  consequences; the command block must stay in shared RAM.

---

## 9. Verifying on a scope / logic analyzer

Probe **SCLK (P9_31)**, **SDI (P9_29)**, **CS (P9_28)**, and the DAC output.

Expected per sample (SPI mode 0):
1. CS goes **LOW**.
2. 16 SCLK pulses; SDI is valid before each **rising** edge, MSB first.
3. CS goes **HIGH** → the DAC latches and the analog output steps.
4. The pattern repeats every sample period (100 µs at 10 kHz).

Quick sanity checks:
- Decode one frame: the top 4 bits should equal `DAC_CTRL_BITS >> 12` (`0x3` by
  default), the low 12 bits your code.
- Feed a slow ramp (`gen_adc.py --waveform ramp --freq 1`) and watch a clean
  staircase/ramp on the analog output.
- Measure the SCLK period to confirm it matches `SPI_SCLK_HZ` (tune per §8a).
- Measure CS-to-CS spacing to confirm the sample rate.

---

*MIT License. Copyright (c) 2026.*
