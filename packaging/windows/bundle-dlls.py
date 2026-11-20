#!/usr/bin/env python3
"""Copies the UCRT64 DLLs the given programs need (and the ones those need) next to them."""
import os
import re
import shutil
import subprocess
import sys

dest = sys.argv[1]
prefix = os.environ.get("MSYSTEM_PREFIX", "/ucrt64")
bin_dir = os.path.join(prefix, "bin")
done = set()


def needs(path):
    out = subprocess.run(["ldd", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*(\S+\.dll) => (\S+)", line)
        if m and m.group(2).lower().startswith(bin_dir.lower()):
            yield m.group(1), m.group(2)
        elif m and "/ucrt64/" in m.group(2).lower():
            yield m.group(1), m.group(2)


def walk(path):
    for name, full in needs(path):
        if name.lower() in done:
            continue
        done.add(name.lower())
        target = os.path.join(dest, name)
        if not os.path.exists(target):
            win = subprocess.run(["cygpath", "-w", full], capture_output=True, text=True).stdout.strip() or full
            shutil.copy(win, target)
        walk(target)


# Programs, and every DLL windeployqt already placed (plugins need their own libraries too).
paths = list(sys.argv[2:])
for root, _, files in os.walk(dest):
    paths += [os.path.join(root, f) for f in files if f.lower().endswith(".dll")]
for p in paths:
    walk(p)
