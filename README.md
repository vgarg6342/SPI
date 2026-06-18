# PRU Single-DAC Player (BeagleBone Black)

Play a table of 12-bit samples out of one SPI DAC at a fixed sample rate, using
PRU0 on the AM335x. The PRU copies the samples **from DDR itself**, frames each
into a 16-bit SPI word (MCP4921-style: 4 control bits + 12 data bits), and
shifts it out on **SPI mode 0** with a compile-time bit clock and a compile-time
sample rate paced by `__delay_cycles()` (no IEP timer).

```
adc.txt ──▶ ARM (frame + write to DDR) ──▶ DDR buffer ──▶ PRU (copy + shift) ──▶ DAC
                                                              │
                                                       heartbeat ◀── ARM watchdog
```

This is a **definitive, single-purpose** build: one DAC, one mode (0), one fixed
clock, one example. No full-duplex, no parallel multi-DAC, no runtime mode/speed
knobs. To change behaviour you edit a macro and rebuild.

---

## Why these design choices

| Requirement | How it's met |
|---|---|
| DDR copy done by the PRU | PRU pulls samples DDR→local SRAM in 512-sample chunks over its OCP master port (`ddr_to_local()` in [pru/pru_spi_fw.c](pru/pru_spi_fw.c)). |
| Only relevant mode-0 SPI | Single MOSI/SDI bit-bang, idle-low clock, sample-on-rising-edge. Everything else removed. |
| 16-bit word / 12-bit DAC | `DAC_FRAME()` applies `DAC_CTRL_BITS` (default `0x3000`) to a 12-bit code in [include/pru_spi_common.h](include/pru_spi_common.h). |
| Detect a crashed / "stuck busy" PRU | PRU bumps `heartbeat` every sample; `pru_dac_play()` returns `PRU_DAC_ERR_HUNG` if it freezes, and reports the remoteproc `state`. |
| Simpler ARM calling | `pru_dac_init()` → `pru_dac_play()` → `pru_dac_close()`. That's it. |
| Single DAC, adc.txt @ 10k S/s, duration macro | `arm/dac_load.c` reads `adc.txt`; `DAC_SAMPLE_RATE_HZ` and `STREAM_DURATION_SEC` are macros. |

---

## Hardware wiring (single DAC)

| Signal | PRU0 R30 bit | Header pin | DAC pin |
|--------|--------------|------------|---------|
| SCLK   | bit 0 | **P9_31** | SCK |
| SDI    | bit 1 | **P9_29** | SDI / DIN / MOSI |
| CS     | bit 3 | **P9_28** | CS / SYNC (active LOW) |
| GND    | —     | P9_1/P9_2 | VSS / AGND |

Tie the DAC's **LDAC** pin LOW so the output updates when CS rises at the end of
each 16-bit word. There is **no MISO** — SPI DACs are write-only.

---

## Quick start

> **This repo is edit-only on your PC.** Compilers (`clpru`, `gcc`) and the board
> live elsewhere. Sync the sources to the BeagleBone, then build *on the board*.

```bash
# 0) One-time board prep: reserve DDR + free the pins in /boot/uEnv.txt,
#    then reboot.  See USER_MANUAL.md §3.

# 1) From the PC that can reach the board: push the sources
bash ./scripts/sync_to_bbb.sh                 # debian@192.168.7.2 by default

# 2) On the BeagleBone: build + deploy (loads firmware, muxes pins, starts PRU)
ssh debian@192.168.7.2
cd /home/debian/DERIC_testing
make
sudo ./scripts/deploy.sh

# 3) Make a test waveform and play it
python3 scripts/gen_adc.py --rate 10000 --duration 5 --freq 100 --out adc.txt
sudo ./arm/dac_load --file adc.txt --rate 10000 --duration 5
```

Check the PRU is alive at any time:

```bash
sudo ./arm/dac_load --status        # prints remoteproc state + ALIVE/HUNG
```

---

## Configuration at a glance

All in [include/pru_spi_common.h](include/pru_spi_common.h) (compile-time → rebuild firmware after changing):

| Macro | Default | Meaning |
|---|---|---|
| `SPI_SCLK_HZ` | `1000000` | SPI bit clock (Hz). `__delay_cycles()` needs a constant. |
| `DAC_SAMPLE_RATE_HZ` | `10000` | Samples per second — **fixed at compile time** (PRU paces with `__delay_cycles()`, which needs a constant). |
| `STREAM_DURATION_SEC` | `5` | Transmission length → sizes the DDR buffer and caps a run. |
| `DAC_CTRL_BITS` | `0x3000` | The 4 control bits OR'd into every 12-bit code. |
| `DAC_VALUE_MAX` | `0x0FFF` | 12-bit full scale (4095). |
| `DDR_BUF_PHYS_BASE` | `0x9F000000` | Physical address of the DDR buffer (**must be reserved**). |

`STREAM_MAX_SAMPLES = DAC_SAMPLE_RATE_HZ * STREAM_DURATION_SEC` is both the DDR
buffer size (× 2 bytes) and the maximum number of samples a single play can carry.

---

## ARM API ([arm/pru_spi.h](arm/pru_spi.h))

```c
int  pru_dac_init(void);
int  pru_dac_play(const uint16_t *codes, uint32_t num_samples,
                  uint32_t timeout_ms);  // blocks; rate is compile-time
int  pru_dac_is_alive(void);            // 1 alive, 0 hung
int  pru_dac_get_state(char *buf, size_t n);   // remoteproc state string
uint32_t pru_dac_capacity_samples(void);
const char *pru_dac_strerror(int err);
void pru_dac_close(void);
```

Minimal program:

```c
pru_dac_init();
pru_dac_play(codes, n, 0);              // frames + DDR + PRU + watchdog
pru_dac_close();
```

---

## Crash / "stuck busy" detection

The old symptom — *the PRU shows busy forever after a code change* — is now
caught. The firmware increments `cmd->heartbeat` on **every sample** (and on
every idle poll). `pru_dac_play()` watches it: if the heartbeat stops advancing
for longer than the watchdog window (`WATCHDOG_FLOOR_MS` = 300 ms, or ~5 sample
periods if larger) while the status is not `DONE`, it stops waiting and returns
`PRU_DAC_ERR_HUNG`, printing the remoteproc `state` and a pointer to `dmesg`.
See USER_MANUAL.md §6.

---

## Repo layout

```
include/pru_spi_common.h   shared ARM/PRU defs (the single source of truth)
pru/pru_spi_fw.c           PRU0 firmware (DDR copy + mode-0 shift + heartbeat)
pru/AM335x_PRU.cmd         PRU linker script
pru/resource_table.h       empty remoteproc resource table
arm/pru_spi.{c,h}          ARM library
arm/dac_load.c             the one example
scripts/setup_pins.sh      mux P9_31/P9_29/P9_28
scripts/deploy.sh          install firmware, mux pins, start PRU (on board)
scripts/sync_to_bbb.sh     rsync sources to the board (from the PC)
scripts/gen_adc.py         generate a test adc.txt
USER_MANUAL.md             full setup, uEnv.txt, configuration, troubleshooting
```

See **[USER_MANUAL.md](USER_MANUAL.md)** for the complete setup (especially the
`uEnv.txt` DDR reservation), every configuration option, and troubleshooting.

MIT License.
