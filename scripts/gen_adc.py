#!/usr/bin/env python3
"""
gen_adc.py - Generate adc.txt for the single-DAC player.

Emits one 12-bit DAC code (0..4095) per line - exactly what dac_load reads.
By default it writes one full sine period; use --waveform for ramp/square/tri.

Runs on the dev PC (no BeagleBone needed). Then:
    sudo ./arm/dac_load --file adc.txt --rate 10000

Examples:
    python3 scripts/gen_adc.py                            # 1 kHz sine, 50000 samples
    python3 scripts/gen_adc.py --rate 10000 --freq 100 --duration 5
    python3 scripts/gen_adc.py --waveform square --out sq.txt
"""

import argparse
import math

DAC_MAX = 4095          # 12-bit full scale


def main():
    ap = argparse.ArgumentParser(description="Generate a single-DAC adc.txt")
    ap.add_argument("--rate", type=int, default=10000,
                    help="sample rate in Hz (default 10000)")
    ap.add_argument("--duration", type=float, default=5.0,
                    help="length in seconds (default 5)")
    ap.add_argument("--freq", type=float, default=1000.0,
                    help="waveform frequency in Hz (default 1000)")
    ap.add_argument("--waveform", choices=["sine", "ramp", "square", "tri"],
                    default="sine", help="waveform shape (default sine)")
    ap.add_argument("--amplitude", type=int, default=DAC_MAX,
                    help="peak-to-peak amplitude in codes (default 4095)")
    ap.add_argument("--offset", type=int, default=None,
                    help="DC midpoint in codes (default amplitude/2)")
    ap.add_argument("--out", default="adc.txt", help="output file (default adc.txt)")
    args = ap.parse_args()

    if args.rate <= 0 or args.duration <= 0:
        ap.error("--rate and --duration must be > 0")

    n = int(round(args.rate * args.duration))
    amp = max(0, min(args.amplitude, DAC_MAX))
    offset = args.offset if args.offset is not None else amp // 2
    half = amp / 2.0

    def clamp(x):
        return max(0, min(DAC_MAX, int(round(x))))

    def sample(i):
        phase = (args.freq * i / args.rate) % 1.0      # 0..1 within one period
        if args.waveform == "sine":
            return offset + half * math.sin(2.0 * math.pi * phase)
        if args.waveform == "ramp":
            return offset - half + amp * phase
        if args.waveform == "square":
            return offset + half if phase < 0.5 else offset - half
        # triangle
        return offset - half + amp * (2 * phase if phase < 0.5 else 2 * (1 - phase))

    with open(args.out, "w") as f:
        f.write("# adc.txt - single-DAC 12-bit codes (one per line)\n")
        f.write("# waveform=%s rate=%d freq=%g duration=%g amp=%d offset=%d\n"
                % (args.waveform, args.rate, args.freq, args.duration, amp, offset))
        for i in range(n):
            f.write("%d\n" % clamp(sample(i)))

    print("Wrote %d samples to %s (%.3f s of %s @ %g Hz)"
          % (n, args.out, args.duration, args.waveform, args.freq))


if __name__ == "__main__":
    main()
