#!/usr/bin/env python3
"""Palm T|X with USB HotSync up: the top bar's sync-start presses HotSync on the cable that is there, as the
Connections menu's hotsync does, and does not replug it. Needs a T|X session (private)."""
import argparse
from pathlib import Path
import runpy
import shutil
import time

ROOT = Path(__file__).resolve().parents[1]


def until(client, command, expected, timeout=60):
    end = time.monotonic() + timeout
    while True:
        ok, code, message = client.request(command)
        if ok and message == expected:
            return
        assert time.monotonic() < end, f"{command}: wanted {expected!r}, got {message!r}"
        time.sleep(0.2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir()
    copy = evidence / args.session.name
    if args.session.is_dir():
        shutil.copytree(args.session, copy, ignore=shutil.ignore_patterns("*.tmp-*"))
    else:
        shutil.copy2(args.session, copy)
    client_type = runpy.run_path(str(ROOT / "tests/core-control.py"))["Client"]
    client = client_type(ROOT / "out/frond", [copy, "--sync-folder", evidence / "sync"], evidence, "core")
    try:
        client.until_ok("guest-ready")
        client.ok("usb-mode", "sync")
        until(client, "usb-state", "sync running")
        # The Connections menu's button.
        client.ok("hotsync")
        until(client, "usb-state", "sync running")
        time.sleep(1)
        # The top bar's: no replug, the press goes in at once.
        message = client.ok("sync-start", "local")
        assert message == "HotSync button pressed.", message
        assert client.ok("usb-state") == "sync running"
        print("PASS top bar sync-start presses over the plugged-in USB cable")
        client.quit()
    finally:
        client.close()


if __name__ == "__main__":
    main()
