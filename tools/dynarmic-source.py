#!/usr/bin/env python3
"""Exports the pinned Dynarmic source and its externals from a git checkout into
build/dynarmic-trial/src and applies patches/dynarmic/*.patch.

Usage: dynarmic-source.py DONOR [DEST]. DONOR is any Dynarmic clone holding the revisions
in patches/dynarmic/source-lock.json, submodules included. It is only read, except that
revisions it lacks are fetched into it from the lock's URLs (DYNARMIC_MIRROR, a local
clone with the same layout, stands in for them). A missing DONOR is made that way."""
import json
import os
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
lock = json.loads((root / "patches/dynarmic/source-lock.json").read_text())
donor = pathlib.Path(sys.argv[1]).resolve()
dest = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else root / "build/dynarmic-trial/src").resolve()
mirror = os.environ.get("DYNARMIC_MIRROR")


def has(repo, rev):
    return (repo / ".git").exists() and subprocess.run(
        ["git", "-C", str(repo), "cat-file", "-e", rev + "^{commit}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def fetch(repo, rev, url):
    if has(repo, rev):
        return
    if not (repo / ".git").exists():
        repo.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
    print("fetching", rev[:12], "from", url)
    subprocess.run(["git", "-C", str(repo), "fetch", "-q", "--depth", "1", url, rev], check=True)


def export(repo, rev, out):
    out.mkdir(parents=True, exist_ok=True)
    tar = subprocess.run(["git", "-C", str(repo), "archive", "--format=tar", rev],
                         check=True, stdout=subprocess.PIPE).stdout
    subprocess.run(["tar", "-x", "-C", str(out)], input=tar, check=True)


fetch(donor, lock["revision"], mirror or lock["url"])
for path, rev in lock["dependencies"].items():
    fetch(donor / path, rev, os.path.join(mirror, path) if mirror else lock["urls"][path])

if dest.exists():
    shutil.rmtree(dest)
export(donor, lock["revision"], dest)
for path, rev in lock["dependencies"].items():
    export(donor / path, rev, dest / path)
for patch in sorted((root / "patches/dynarmic").glob("*.patch")):
    subprocess.run(["patch", "-p1", "-s", "-d", str(dest), "-i", str(patch)], check=True)
    print("applied", patch.name)
(dest / ".frond-source").write_text(lock["revision"] + "\n")
print("dynarmic source in", dest)
