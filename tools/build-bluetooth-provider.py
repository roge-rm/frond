#!/usr/bin/env python3
"""Explicitly build the optional local BlueZ H4 provider; never install it."""
import argparse
from pathlib import Path
import subprocess

REVISION = "82667d713949447d7c94725432c61b33689ae68a"
SOURCE = "https://github.com/bluez/bluez.git"
ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "tools/bluez-bdaddr.patch"


def run(*args, cwd):
    subprocess.run(args, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "build/bluetooth-provider")
    parser.add_argument("--fetch", action="store_true", help="explicitly download the pinned upstream source into an empty directory")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    source = args.source.resolve()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.fetch:
        if source.exists() and any(source.iterdir()):
            parser.error("--fetch requires an empty source directory")
        source.mkdir(parents=True, exist_ok=True)
        run("git", "init", cwd=source)
        run("git", "remote", "add", "origin", SOURCE, cwd=source)
        run("git", "fetch", "--depth=1", "origin", REVISION, cwd=source)
        run("git", "checkout", "--detach", "FETCH_HEAD", cwd=source)
    if not (source / ".git").exists():
        parser.error("source checkout missing; use --fetch to download it explicitly")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if revision != REVISION:
        parser.error("source must be pinned to " + REVISION)
    # Idempotent only for this exact patch; never reset or discard local work.
    applied = subprocess.run(["git", "apply", "--reverse", "--check", str(PATCH)], cwd=source, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not applied:
        run("git", "apply", "--check", str(PATCH), cwd=source)
        run("git", "apply", str(PATCH), cwd=source)
    changes = subprocess.check_output(["git", "diff", "--no-ext-diff", "--no-color", "HEAD"], cwd=source, text=True)
    if changes != PATCH.read_text():
        parser.error("checkout contains tracked changes beyond the qualified extension")
    run("./bootstrap", cwd=source)
    disabled = ["systemd", "udev", "cups", "obex", "client", "tools", "monitor", "manpages", "datafiles", "mesh", "midi", "bap", "mcp", "vcp", "bass", "csip", "asha", "network", "hid", "hog", "a2dp", "avrcp", "library"]
    run("./configure", "--enable-testing", *("--disable-" + name for name in disabled), cwd=source)
    run("make", "-j" + str(args.jobs), "emulator/btvirt", cwd=source)
    print("Built optional provider:", source / "emulator/btvirt")
    print("No installation, service or host adapter changes were made.")


if __name__ == "__main__":
    main()
