#!/usr/bin/env python3
"""
gen_adc.py - Generate adc.txt: a 4-column sine table for dac_stream.

Each output line is four 12-bit DAC codes (0..4095), one per DAC, separated by
spaces. The four columns are the same sine wave phase-shifted by 0/90/180/270
degrees, so on a scope you see four sines in quadrature.

This runs on the dev PC (no BeagleBone needed). Feed the result to:
    sudo ./arm/dac_stream --file adc.txt --rate 10000

Examples:
    python3 scripts/gen_adc.py                       # 1 period, 200 samples
    python3 scripts/gen_adc.py --samples 1000 --periods 5 --out adc.txt
"""

import argparse
import math

DAC_MAX = 4095          # 12-bit full scale


def main():
    ap = argparse.ArgumentParser(description="Generate a 4-DAC sine adc.txt")
    ap.add_argument("--samples", type=int, default=200,
                    help="total number of samples (lines) to emit (default 200)")
    ap.add_argument("--periods", type=float, default=1.0,
                    help="number of full sine periods across all samples (default 1)")
    ap.add_argument("--amplitude", type=int, default=DAC_MAX,
                    help="peak-to-peak amplitude in codes (default 4095)")
    ap.add_argument("--offset", type=int, default=None,
                    help="DC offset/midpoint in codes (default amplitude/2)")
    ap.add_argument("--phases", type=float, nargs=4,
                    default=[0.0, 90.0, 180.0, 270.0],
                    help="per-DAC phase in degrees (default 0 90 180 270)")
    ap.add_argument("--out", default="adc.txt", help="output file (default adc.txt)")
    args = ap.parse_args()

    if args.samples <= 0:
        ap.error("--samples must be > 0")

    amp = max(0, min(args.amplitude, DAC_MAX))
    offset = args.offset if args.offset is not None else amp // 2
    half = amp / 2.0
    phases = [math.radians(p) for p in args.phases]

    def clamp(x):
        return max(0, min(DAC_MAX, int(round(x))))

    with open(args.out, "w") as f:
        f.write("# adc.txt - 4-DAC sine table (DAC0 DAC1 DAC2 DAC3), 12-bit codes\n")
        f.write("# samples=%d periods=%g amplitude=%d offset=%d\n"
                % (args.samples, args.periods, amp, offset))
        for n in range(args.samples):
            theta = 2.0 * math.pi * args.periods * n / args.samples
            vals = [clamp(offset + half * math.sin(theta + ph)) for ph in phases]
            f.write("%d %d %d %d\n" % tuple(vals))

    print("Wrote %d samples to %s" % (args.samples, args.out))


if __name__ == "__main__":
    main()
