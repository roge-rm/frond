#!/usr/bin/env python3
"""The printer commands through each core's desktop control socket, as the desktop and Android apps send
them: irprinter status|save PATH|clear on out/frond (an ARM smoke session) and on out/frond-68k (the Dana
ROM), and the Dana's USB printer command on both. Each must get past the socket's argument check.

Private firmware and sessions: --arm-session, else FROND_ARM_SESSION, else the palm-tx smoke machine;
--dana-rom, else AlphaSmart/Dana/AlphaSmart-Dana-1.5.rom in FROND_DEVICE_IMAGES. A side with neither is
skipped; with both skipped the test fails. Scratch goes in build/printer-control-*.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot  # noqa: E402

spec = importlib.util.spec_from_file_location('core_control', ROOT / 'tests/core-control.py')
core_control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(core_control)

STATUS = re.compile(r'(attached|off) \S+ bytes=\d+( receiving=\d+)? last=\S+ idle=-?\d+$')


def check_irprinter(c, root):
    """Printer mode on, then each irprinter action with its argument."""
    assert c.ok('irda-mode', 'printer', code='infrared') == 'printer idle'
    text = c.ok('irprinter', 'status', code='irprinter')
    assert text == 'attached ready bytes=0 receiving=0 last=none idle=-1', text
    text = c.fails('irprinter', f'save {root / "nothing.raw"}', code='irprinter')
    assert text.startswith('Nothing to save. attached ready'), text
    assert not (root / 'nothing.raw').exists()
    assert STATUS.search(c.ok('irprinter', 'clear', code='irprinter'))
    assert c.fails('irprinter', 'print', code='irprinter').startswith('Use status, save PATH or clear.')
    assert c.ok('irda-mode', 'off', code='infrared') == 'off'


def check_arm(core, session, root):
    c = core_control.Client(core, [session], root, 'arm')
    try:
        check_irprinter(c, root)
        # The ARM devices have no USB printer: refused as unsupported, not as an argument it can't take.
        c.fails('printer', 'status', code='unsupported')
        c.fails('printer', 'attach on', code='unsupported')
        c.quit()
    finally:
        c.close()


def check_dana(core, rom, root):
    c = core_control.Client(core, ['--rom', rom, '--device', 'alphasmart-dana'], root, 'dana')
    try:
        assert c.ok('device-state') == 'alphasmart-dana'
        check_irprinter(c, root)
        assert STATUS.search(c.ok('printer', 'status', code='printer'))
        ok, code, text = c.request('printer', 'attach on')
        assert code != 'argument', text
        c.request('printer', 'attach off')
        c.quit()
    finally:
        c.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arm-session', type=Path)
    parser.add_argument('--dana-rom', type=Path)
    args = parser.parse_args()
    session = core_control.arm_session(args.arm_session)
    rom = args.dana_rom or palm_boot.palm_images() / 'AlphaSmart/Dana/AlphaSmart-Dana-1.5.rom'
    (ROOT / 'build').mkdir(exist_ok=True)
    failed = ran = 0
    with tempfile.TemporaryDirectory(prefix='printer-control-', dir=ROOT / 'build') as work:
        root = Path(work).resolve()
        for name, ready, run in (('frond', session and session.exists(),
                                  lambda: check_arm(ROOT / 'out/frond', session.resolve(), root)),
                                 ('frond-68k', rom.is_file(), lambda: check_dana(ROOT / 'out/frond-68k', rom.resolve(), root))):
            if not ready:
                print(f'printer-control {name}: skipped, no {"ARM smoke session" if name == "frond" else f"Dana ROM at {rom}"}')
                continue
            ran += 1
            try:
                run()
                print(f'printer-control {name}: ok', flush=True)
            except AssertionError as error:
                failed += 1
                print(f'printer-control {name}: FAIL, {error}', flush=True)
    if not ran:
        print('printer-control: FAIL, every side was skipped, nothing was checked')
        return 1
    print('printer-control: ' + ('FAIL' if failed else 'ok'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
