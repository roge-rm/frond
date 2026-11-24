#!/usr/bin/env python3
"""Check Zodiac HotSync commands on the same control socket Android uses."""
import argparse
from pathlib import Path
import runpy
import time

ROOT = Path(__file__).resolve().parents[1]


def wait_for(client, command, expected):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        ok, code, message = client.request(command)
        if ok and expected in (code, message):
            return
        time.sleep(0.05)
    raise AssertionError(f"{command}: expected {expected!r}, got {code!r} {message!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir()
    replay = evidence / "hold.steps"
    replay.write_text("0 pause\n")

    client_type = runpy.run_path(str(ROOT / "tests/core-control.py"))["Client"]
    relay = "tcp:0"
    client = client_type(
        ROOT / "out/frond",
        [args.rom.resolve(), "--input-replay", replay, "--sync-folder", evidence / "sync"],
        evidence, "core")
    try:
        wait_for(client, "replay-state", "paused")
        # A Zodiac starts in its cradle.
        assert client.ok("cradle-setting") == "attached"
        client.ok("cradle", "detached")
        client.ok("usb-mode", "off")
        assert client.fails("hotsync", code="cradle") == "Attach the cradle or USB cable first."
        client.fails("sync-start", "local", code="cradle")
        client.ok("usb-mode", "tcp:0")
        client.fails("sync-start", "local", code="cradle")
        client.ok("usb-mode", "off")
        client.ok("cradle", "attached")
        for route in ("off", relay, "sync"):
            client.ok("usb-mode", "off")
            client.ok("sync-start", "local")
            client.fails("sync-start", "local", code="busy")
            client.events.clear()
            client.ok("usb-mode", route)
            state = client.ok("usb-state")
            if route == relay:
                assert state.startswith("tcp 127.0.0.1:")
            assert ("sync", "failed usb: The Sync Manager USB route changed.") in client.events
            assert client.ok("cradle-button-state") == "up"
            if route == "sync":
                # A new route of the same kind also cancels the old press.
                client.ok("sync-start", "local")
            print(f"PASS pending press cancelled by {route.split(':', 1)[0]}")
        client.ok("usb-mode", relay)
        client.ok("replay-continue")
        wait_for(client, "cradle-state", "attached")
        assert client.ok("cradle-button-state") == "up"
        client.ok("hotsync")
        assert client.ok("cradle-button-state") == "down"
        print("PASS detached feedback and explicit HotSync over the relay cable")
        client.quit()
    finally:
        client.close()


if __name__ == "__main__":
    main()
