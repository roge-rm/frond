#!/usr/bin/env python3
"""Check TX bring-up input validation without starting an interactive emulator."""

import argparse
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("nor")
parser.add_argument("nand")
args = parser.parse_args()

base = [args.binary, "--device", "palm-tx", args.nor]
cases = [
    ([], "requires a 16 MiB"),
    (["--nand", args.nor], "invalid NAND size"),
    (["--nand", args.nand, "--ram-size", "16"], "unsupported RAM size"),
    (["--nand", args.nand, "--display", "320x320"], "unsupported display mode"),
    (["--nand", args.nand, "--run-for", "0"], "positive duration"),
]

for flags, expected in cases:
    result = subprocess.run(base + flags, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=10)
    if result.returncode != 1 or expected not in result.stdout:
        raise SystemExit(f"FAIL {flags}: exit={result.returncode}\n{result.stdout}")
    print(f"PASS: {expected}")
