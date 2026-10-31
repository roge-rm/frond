#!/usr/bin/env python3
"""Check that invalid input replays fail before guest execution."""

import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("nor")
parser.add_argument("nand")
args = parser.parse_args()
command = [args.binary, "--device", "palm-tx", args.nor, "--nand", args.nand,
           "--no-sound", "--run-for", "1"]
cases = {
    "negative-time": "-1 pen-up\n",
    "out-of-order": "100 pen-up\n50 pen-up\n",
    "off-screen": "1 pen-down 320 0\n",
    "unknown-key": "1 key-down bogus\n",
    "missing-coordinate": "1 pen-down 12\n",
    "duration": "2000 pen-up\n",
}

with tempfile.TemporaryDirectory() as directory:
    for name, body in cases.items():
        replay = Path(directory) / name
        replay.write_text(body)
        result = subprocess.run(command + ["--input-replay", str(replay)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        if result.returncode != 1 or not any(
            message in result.stdout for message in ("invalid replay event", "extends beyond")
        ):
            raise SystemExit(f"FAIL {name}: exit={result.returncode}\n{result.stdout}")
        print(f"PASS: {name}")
