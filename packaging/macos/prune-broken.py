#!/usr/bin/env python3
"""Removes plugins and libraries in the app whose @rpath libraries aren't in it, until none are left."""
import os, re, subprocess, sys

app = sys.argv[1]
contents = os.path.join(app, "Contents")

def files():
    for root, _, names in os.walk(contents):
        for n in names:
            p = os.path.join(root, n)
            if n.endswith((".dylib", ".so")) and not os.path.islink(p):
                yield p

def have():
    return {os.path.basename(p) for p in files()}

def rpath_deps(path):
    out = subprocess.run(["otool", "-L", path], capture_output=True, text=True).stdout.splitlines()[1:]
    return [os.path.basename(m.group(1)) for m in (re.match(r"\s+@rpath/(\S+) \(", l) for l in out) if m]

changed = True
while changed:
    changed = False
    present = have()
    # Qt frameworks are found by path, so only flat dylib names are checked.
    for p in list(files()):
        missing = [d for d in rpath_deps(p) if ".framework/" not in d and d not in present]
        if missing:
            print("removing", p, "needs", missing)
            os.remove(p)
            changed = True
