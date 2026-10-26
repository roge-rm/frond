#!/usr/bin/env python3
"""Stage the pinned native core and apply Frond's changes without editing the submodule."""

from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "vendor/cloudpilot"
REVISION = "064d5d578bcd1f9072de384d05aa217f83db2f7f"
DESTINATION = ROOT / "build/source"
PATCHES = sorted((ROOT / "patches").glob("*.patch"))

if not (UPSTREAM / ".git").exists():
    raise SystemExit("Initialize source first: git submodule update --init vendor/cloudpilot")

actual = subprocess.check_output(["git", "-C", str(UPSTREAM), "rev-parse", "HEAD"], text=True).strip()
if actual != REVISION:
    raise SystemExit(f"Expected upstream {REVISION}, got {actual}; restore the pinned submodule")

if DESTINATION.exists():
    raise SystemExit("Prepared source already exists. Export local edits, then run make clean before restaging.")

DESTINATION.parent.mkdir(parents=True, exist_ok=True)
try:
    with tempfile.TemporaryFile() as archive:
        subprocess.run(["git", "-C", str(UPSTREAM), "archive", REVISION,
                        "src/common", "src/uarm", "src/argparse", "src/Makefile.common",
                        "LICENSE"], stdout=archive, check=True)
        archive.seek(0)
        with tarfile.open(fileobj=archive) as sources:
            sources.extractall(DESTINATION, filter="data")
    for patch in PATCHES:
        subprocess.run(["git", "apply", "--directory=build/source", str(patch)], cwd=ROOT, check=True)
    (DESTINATION / ".prepared").touch()
except BaseException:
    shutil.rmtree(DESTINATION, ignore_errors=True)
    raise

print(f"Prepared native core from {REVISION}")
