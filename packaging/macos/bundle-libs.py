#!/usr/bin/env python3
"""Copies the non-system dylibs a Mach-O binary needs into the app's Frameworks and points it at them."""
import os, re, shutil, subprocess, sys

app = sys.argv[1]
frameworks = os.path.join(app, "Contents", "Frameworks")
os.makedirs(frameworks, exist_ok=True)

def deps(path):
    out = subprocess.run(["otool", "-L", path], capture_output=True, text=True, check=True).stdout.splitlines()[1:]
    return [m.group(1) for m in (re.match(r"\s+(\S+) \(", l) for l in out) if m]

def system(lib):
    return lib.startswith(("/usr/lib/", "/System/")) or lib.startswith("@")

done = {}

def bundle(path, target_dir_rel):
    for lib in deps(path):
        if system(lib):
            continue
        real = os.path.realpath(lib)
        name = os.path.basename(real)
        dest = os.path.join(frameworks, name)
        if name not in done:
            shutil.copy(real, dest)
            os.chmod(dest, 0o755)
            done[name] = dest
            subprocess.run(["install_name_tool", "-id", "@loader_path/" + name, dest], check=True)
            bundle(dest, "@loader_path/")
        subprocess.run(["install_name_tool", "-change", lib, target_dir_rel + name, path], check=True)

for binary in sys.argv[2:]:
    bundle(binary, "@executable_path/../Frameworks/")
