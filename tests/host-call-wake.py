#!/usr/bin/env python3
"""A readiness check is a look; a host call may wake the UI task (patch 0244). Runs the T|X with
Memo Pad in front and the Zire 72 under its Reminder modal with FROND_TRACE_WAKE=1, which makes the
core log each wake it makes, and counts them. guest-ready must make none in either state. On the T|X
an install with Memo Pad in front must work and make a wake; on the Zire the refused install makes
at most one. Sessions: FROND_ARM_SESSIONS/DEVICE or build/smoke/machines/DEVICE, else the device is
skipped (SKIPPED; FROND_REQUIRE_SESSIONS=1 makes that a failure). FROND_CORE picks the core."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('core_control', ROOT / 'tests/core-control.py')
core_control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(core_control)
DEVICES = ("palm-tx", "palm-zire-72")



def run(name, session, root):
    core = Path(os.environ.get('FROND_CORE') or ROOT / 'out/frond')
    os.environ['FROND_TRACE_WAKE'] = '1'
    c = core_control.Client(core, [session, '--device', name], root, name)
    log = root / f'{name}.log'

    def wakes():
        return log.read_text(errors='replace').count('native: host wake')

    notes = []
    try:
        if name == 'palm-tx':
            c.until_ok('guest-ready')
            for _ in range(30):
                ok, code, text = c.request('launch-app', 'Memo Pad')
                if ok:
                    break
                time.sleep(2)
            else:
                raise AssertionError(f'Memo Pad did not launch: {code} {text}')
            time.sleep(5)
        else:
            # The Reminder comes up a few seconds in and holds the guest in a modal.
            time.sleep(30)
        before = wakes()
        time.sleep(5)
        idle = wakes() - before
        before = wakes()
        ok, code, text = c.request('guest-ready')
        looked = wakes() - before
        notes.append(f'idle wakes {idle}, guest-ready {"ok" if ok else code} with {looked} wakes')
        if looked:
            raise AssertionError(f'guest-ready woke the UI task {looked} time(s)')
        before = wakes()
        started = time.monotonic()
        ok, code, text = c.request('install', core_control.PATTERN)
        took = time.monotonic() - started
        called = wakes() - before
        notes.append(f'install {"ok" if ok else code} with {called} wakes in {took:.1f} s')
        if name == 'palm-tx' and (not ok or called < 1):
            raise AssertionError(f'the install with Memo Pad in front: {code} {text}, {called} wakes')
        if called > 1 + idle:
            raise AssertionError(f'the install woke the UI task {called} times')
        c.quit()
    finally:
        c.close()
    return '; '.join(notes)


def main():
    folder = os.environ.get('FROND_ARM_SESSIONS')
    required = os.environ.get('FROND_REQUIRE_SESSIONS') == '1'
    failed = skipped = 0
    with tempfile.TemporaryDirectory(prefix='host-call-wake-', dir=ROOT / 'build') as work:
        for name in DEVICES:
            session = next((p for p in ([Path(folder) / name] if folder else []) + [ROOT / 'build/smoke/machines' / name]
                            if p.exists()), None)
            if not session:
                print(f'host-call wake: {name} SKIPPED, no session')
                skipped += 1
                continue
            copy = Path(work) / name
            import shutil
            shutil.copytree(session, copy)
            frond = sorted(copy.glob('*.frond'))
            try:
                print(f'host-call wake: {name} ok, {run(name, frond[0] if frond else copy, Path(work))}', flush=True)
            except AssertionError as error:
                failed += 1
                print(f'host-call wake: {name} FAIL, {error}', flush=True)
    return 1 if failed or (required and skipped) else 0


if __name__ == '__main__':
    sys.exit(main())
