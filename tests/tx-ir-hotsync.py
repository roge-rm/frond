#!/usr/bin/env python3
"""IR HotSync on a Palm T|X: the core runs with --irda sync, the HotSync app's Local list is set to IR to a
PC/Handheld and the HotSync icon tapped, and the Sync Manager backs the T|X up over infrared. Runs Uncapped. By
default it passes once the user, personal data and the first --databases databases have gone over IR (a couple of
minutes); --full waits for the whole sync, about half an hour for a T|X, as a session that hasn't synced gets a new
user each run and backs everything up. Needs a T|X session (private)."""
import argparse
from pathlib import Path
import re
import runpy
import shutil
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--full", action="store_true", help="wait for the whole sync to finish")
    parser.add_argument("--databases", type=int, default=10, help="databases to back up before passing (default 10)")
    parser.add_argument("--timeout", type=int, help="seconds to wait (default 600, or 3300 with --full)")
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True)
    copy = evidence / args.session.name
    if args.session.is_dir():
        shutil.copytree(args.session, copy, ignore=shutil.ignore_patterns("*.tmp-*"))
    else:
        shutil.copy2(args.session, copy)
    sync = evidence / "sync"
    client_type = runpy.run_path(str(ROOT / "tests/core-control.py"))["Client"]
    client = client_type(ROOT / "out/frond", [copy, "--sync-folder", sync, "--irda", "sync"], evidence, "core")
    replays = 0

    def replay(lines):
        nonlocal replays
        replays += 1
        path = evidence / f"input-{replays}.replay"
        path.write_text("\n".join(lines) + "\n")
        client.ok("input-replay", str(path))
        end = time.monotonic() + 20
        while client.request("replay-state")[1] != "finished":
            assert time.monotonic() < end, "input replay did not finish"
            time.sleep(0.2)

    def tap(x, y):
        replay([f"0 pen-down {x} {y}", "150 pen-up"])
        time.sleep(0.8)

    def frame(name):
        replay([f'0 snapshot "{evidence / name}"'])

    try:
        client.until_ok("guest-ready")
        state = client.ok("irda-state")
        assert state == "sync idle; waiting infrared", state
        client.ok("speed", "uncapped")
        client.ok("launch-app", "HotSync")
        time.sleep(4)
        # The list under the icon, IR to a PC/Handheld, then the HotSync icon.
        tap(150, 229)
        tap(150, 251)
        frame("ir-chosen.png")
        tap(160, 165)
        seen = []
        timeout = args.timeout or (3300 if args.full else 600)
        end = time.monotonic() + timeout
        backup = None
        while True:
            state = client.ok("irda-state")
            link = state.split(";")[0]
            if not seen or seen[-1] != link:
                seen.append(link)
            if "done infrared" in state or "failed" in state.split(";", 1)[-1].split(":")[0]:
                break
            backup = re.search(r"backup (\d+)/(\d+)", state)
            if not args.full and backup and int(backup.group(1)) >= args.databases:
                break
            assert time.monotonic() < end, f"no IR HotSync result in {timeout} s: {state}"
            time.sleep(2)
        failed = "failed" in state.split(";", 1)[-1].split(":")[0]
        if failed:
            frame("failed.png")
        assert not failed, f"IR HotSync failed: {state}; link went {' > '.join(seen)}"
        if not args.full and backup and "done infrared" not in state:
            frame("backing-up.png")
            assert "sync open" in seen, f"IR HotSync: {state}; link went {' > '.join(seen)}"
            print(f"PASS T|X IR HotSync: {state.split('; ', 1)[1]}; link went {' > '.join(seen)}")
            client.quit()
            return
        # The HotSync app shows its result once it has cleaned up.
        time.sleep(20)
        frame("done.png")
        assert "done infrared" in state and "sync open" in seen, f"IR HotSync: {state}; link went {' > '.join(seen)}"
        counts = re.search(r"(\d+) installed, (\d+) backed up", state)
        assert counts, state
        print(f"PASS T|X IR HotSync: {state.split('; ', 1)[1]}; link went {' > '.join(seen)}")
        client.quit()
    finally:
        client.close()


if __name__ == "__main__":
    main()
