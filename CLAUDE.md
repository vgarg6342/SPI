# CLAUDE.md — Working notes for this repo

## Build / test policy

**Do NOT build, compile, run, configure, or test anything on this machine.**
This machine is **edit-only** (it only has internet, no board attached and no
toolchains). The code is taken from here to a separate PC that can reach the
BeagleBone Black, and is compiled + run **on the BeagleBone** (AM335x PRU). The
toolchains — `clpru` (TI PRU C compiler) and native ARM `gcc` +
pru-software-support headers — live on the BeagleBone.

- Edit source here only. Do not invoke `make`, compilers, `chmod`, or device
  setup from this machine.
- Editor/IDE diagnostics here about missing headers (`pru_spi_common.h not
  found`, unknown `__R30` / `__delay_cycles`, etc.) are EXPECTED and are NOT
  real build errors — those resolve only under the on-device toolchains.
- When something is uncertain about the build/hardware, document it as a caveat
  in `USER_MANUAL.md` (sections 8a/8b) rather than trying to verify it here.

## Deploy target on the BeagleBone

The project is built and run inside:

```
/home/debian/DERIC_testing
```

## Push code to the BeagleBone

From the PC that can reach the board (NOT this machine), use the sync script
(rsync over SSH), then build on the device:

```bash
# On the PC connected to the BeagleBone:
bash ./scripts/sync_to_bbb.sh                       # defaults: debian@192.168.7.2
BBB_HOST=beaglebone.local bash ./scripts/sync_to_bbb.sh   # override host

# Then on the BeagleBone:
ssh debian@192.168.7.2
cd /home/debian/DERIC_testing
make                       # builds PRU firmware (clpru) + ARM code (gcc)
sudo ./scripts/deploy.sh   # install firmware, mux pins, start PRU
```

`sync_to_bbb.sh --build` runs the remote `make`; `--deploy` also runs deploy.
See USER_MANUAL.md §8b for sync caveats and §8a for DDR/timing caveats.

## Layout

This project does ONE thing: play a table of 12-bit DAC codes out of DDR to a
single SPI DAC (MCP4921-style 16-bit frame), SPI mode 0, fixed bit clock. The
PRU copies the samples from DDR itself and paces output with `__delay_cycles()`
(NO IEP timer — `pru_iep.h` is not available on the target).

- `include/pru_spi_common.h` — shared ARM/PRU defs (pins, 64-byte command block,
  clock, DAC framing, sample rate/duration, DDR buffer address).
  **SPI clock + sample rate + duration are all compile-time** (`SPI_SCLK_HZ`,
  `DAC_SAMPLE_RATE_HZ`, `STREAM_DURATION_SEC`); `__delay_cycles()` needs a const.
- `pru/pru_spi_fw.c` — PRU0 firmware: copies samples DDR→local SRAM in chunks,
  shifts each 16-bit word out (mode 0, CS-framed), paces with `__delay_cycles()`;
  bumps a `heartbeat` word every sample so the ARM side can detect a crash/hang.
- `arm/pru_spi.{c,h}` — ARM userspace API: `pru_dac_init/play/is_alive/
  get_state/close`. `pru_dac_play()` frames codes into DDR, kicks the PRU, and
  watches the heartbeat (returns `PRU_DAC_ERR_HUNG` if the PRU stops responding).
- `arm/dac_load.c` — the one example: reads `adc.txt` (one 12-bit code per line)
  and plays it.
- `scripts/` — `sync_to_bbb.sh` (push), `deploy.sh` (on-device), `setup_pins.sh`
  (mux P9_31/P9_29/P9_28), `gen_adc.py` (make a test `adc.txt`).

The DDR buffer lives at a fixed physical address that **must be reserved** by
shrinking the kernel's RAM in `uEnv.txt` (`mem=...`) — see USER_MANUAL.md §3.
