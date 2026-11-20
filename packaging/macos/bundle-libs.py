#!/usr/bin/env python3
"""Makes the app self-contained: copies the Homebrew libraries the cores and everything in the app
need into Contents/Frameworks, and points every reference at them.

usage: bundle-libs.py APP BREW_PREFIX CORE...
"""
import glob, os, re, shutil, subprocess, sys

app, prefix = sys.argv[1], sys.argv[2]
cores = sys.argv[3:]
frameworks = os.path.join(app, "Contents", "Frameworks")
os.makedirs(frameworks, exist_ok=True)
REF = "@executable_path/../Frameworks/"

def deps(path):
    out = subprocess.run(["otool", "-L", path], capture_output=True, text=True).stdout.splitlines()[1:]
    return [m.group(1) for m in (re.match(r"\s+(\S+) \(", l) for l in out) if m]

def system(lib):
    return lib.startswith(("/usr/lib/", "/System/", "@executable_path/", "@loader_path/"))

def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        return f.read(4) in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xfe\xed\xfa\xcf")

def find_brew(name):
    for pattern in (prefix + "/opt/*/lib/" + name, prefix + "/lib/" + name):
        hits = glob.glob(pattern)
        if hits:
            return os.path.realpath(hits[0])
    return None

def copy_in(real):
    dest = os.path.join(frameworks, os.path.basename(real))
    if not os.path.exists(dest):
        shutil.copy(real, dest)
        os.chmod(dest, 0o755)
        subprocess.run(["install_name_tool", "-id", REF + os.path.basename(real), dest], check=True)
    return dest

def retarget(path, old, new):
    subprocess.run(["install_name_tool", "-change", old, new, path], check=True)

def fix(path):
    for lib in deps(path):
        if system(lib):
            continue
        if ".framework/" in lib:
            if lib.startswith("@rpath/"):
                retarget(path, lib, REF + lib[len("@rpath/"):])
            continue
        name = os.path.basename(lib)
        if lib.startswith("@rpath/") and os.path.exists(os.path.join(frameworks, name)):
            real = os.path.join(frameworks, name)
        else:
            real = lib if lib.startswith("/") and os.path.exists(lib) else find_brew(name)
        if real is None:
            print("not found:", lib, "for", path)
            continue
        real = os.path.realpath(real)
        dest = copy_in(real)
        retarget(path, lib, REF + os.path.basename(dest))
        if dest not in seen:
            seen.add(dest)
            queue.append(dest)

seen = set()
queue = [c for c in cores]
for root, _, names in os.walk(os.path.join(app, "Contents")):
    for n in names:
        p = os.path.join(root, n)
        if is_macho(p) and p not in cores:
            queue.append(p)
# Homebrew's SDL2 is sdl2-compat, which loads SDL3 with dlopen when it starts, so it isn't in the link list.
sdl3 = find_brew("libSDL3.0.dylib")
if sdl3:
    queue.append(copy_in(sdl3))
queue = [os.path.abspath(p) if not os.path.isabs(p) else p for p in queue]
while queue:
    fix(queue.pop())
