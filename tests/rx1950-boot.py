#!/usr/bin/env python3
"""Boots the HP iPAQ rx1950's eboot and OS on the S3C2442 machine to the kernel's first debug lines.

    tests/rx1950-boot.py build/rx1950-boot

eboot's lines come from UART0 and the kernel's from its debug text ring (nk.log). Skips when the
firmware isn't on this machine. Set RX1950_FIRMWARE to the folder with CEBL.nbf and CEOS.nbf and
RX1950_OUT to keep the logs somewhere else than rx1950-boot-logs next to the tool.
"""
import os
import subprocess
import sys

FIRMWARE = os.environ.get(
    "RX1950_FIRMWARE", "/srv/downloads/backup/deviceimages/winmobile/HP/iPAQ-rx1950/ipaq_rx1950_wm5")
# What each log has to hold: eboot loading the OS and entering it, then the kernel through OEMInit.
EXPECTED = {
    "uart0.log": [b"InitDebugSerial using UART Serial", b"OEMTranslateBaseAddress 23 80000000 80000000",
                  b"Jump to Physical Address 30100000"],
    "nk.log": [b"Windows CE Kernel for ARM", b"ProcessorType=0920", b"-OEMInit..."],
}
INSTRUCTIONS = os.environ.get("RX1950_INSTRUCTIONS", "400000000")


def main():
    tool = sys.argv[1]
    cebl, ceos = os.path.join(FIRMWARE, "CEBL.nbf"), os.path.join(FIRMWARE, "CEOS.nbf")
    if not (os.path.exists(cebl) and os.path.exists(ceos)):
        print("rx1950-boot: SKIP, no firmware in " + FIRMWARE)
        return 0
    out = os.environ.get("RX1950_OUT") or os.path.join(os.path.dirname(os.path.abspath(tool)), "rx1950-boot-logs")
    os.makedirs(out, mode=0o700, exist_ok=True)
    run = subprocess.run([tool, "--cebl", cebl, "--ceos", ceos, "--out", out, "--instructions", INSTRUCTIONS,
                          "--debug-messages"],
                         capture_output=True, text=True, timeout=600)
    print(run.stdout.strip())
    missing = []
    for name, lines in EXPECTED.items():
        with open(os.path.join(out, name), "rb") as f:
            log = f.read()
        missing += [name + ": " + line.decode() for line in lines if line not in log]
    if run.returncode or missing:
        print("rx1950-boot: FAIL" + (", missing: " + ", ".join(missing) if missing else ""))
        print(run.stderr.strip())
        return 1
    print("rx1950-boot: OK, logs in " + out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
