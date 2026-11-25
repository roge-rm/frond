#!/usr/bin/env python3
"""Cold boot the Zodiac 1 with the 1.1 ROM and check that session models stay distinct."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = Path(os.environ.get('FROND_CORE') or ROOT / 'out/frond').resolve()
ZODIAC1 = 'tapwave-zodiac-1'
ZODIAC2 = 'tapwave-zodiac-2'
sys.path.insert(0, str(ROOT / 'tools'))
from palm_boot import png_rows


def main():
    rom = os.environ.get('ZODIAC_ROM')
    if not rom or not Path(rom).is_file():
        print('zodiac-1-boot: skipped, no ZODIAC_ROM')
        return
    rom = Path(rom).resolve()
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    env.pop('FROND_ZODIAC_STATE', None)
    work_root = ROOT / 'build'
    work_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='zodiac-1-test-', dir=work_root) as temporary:
        work = Path(temporary)

        def run(args, expected, ok=True):
            result = subprocess.run([str(CORE), '--no-sound', *args], cwd=work, env=env,
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=150)
            if (result.returncode == 0) != ok or expected not in result.stdout:
                raise AssertionError(result.stdout[-3000:])
            return result.stdout

        frame = work / 'cold.png'
        boot = run([str(rom), '--device', ZODIAC1, '--engine', 'fast', '--run-for', '60',
                    '--framebuffer-output', str(frame)], 'using RAM size: 33554432 bytes')
        assert f'device: {ZODIAC1} (' in boot and 'LCD=1' in boot, boot[-3000:]
        rows = png_rows(frame)
        assert len(rows) == 320 and all(len(row) == 480 * 4 for row in rows)
        assert len({row[x:x + 3] for row in rows[::8] for x in range(0, len(row), 32)}) > 16
        if evidence := os.environ.get('ZODIAC_1_EVIDENCE'):
            folder = Path(evidence)
            folder.mkdir(mode=0o700, parents=True)
            shutil.copyfile(frame, folder / 'welcome.png')
            (folder / 'boot.log').write_text(boot)

        for model, other, ram in ((ZODIAC1, ZODIAC2, 33554432), (ZODIAC2, ZODIAC1, 134217728)):
            folder = work / model
            folder.mkdir()
            session = folder / (model + '.frond')
            save_script = work / 'save.txt'
            save_script.write_text(f'save-session "{session}"\nquit\n')
            # The shared ROM still defaults to the Zodiac 2. Zodiac 1 is selected explicitly.
            select = ['--device', model] if model == ZODIAC1 else []
            run([str(rom), '--script', str(save_script), *select], f'using RAM size: {ram} bytes')
            assert session.is_file(), 'the console did not save the session'
            reopened = run([str(session), '--run-for', '1'], f'using RAM size: {ram} bytes')
            assert f'device: {model} (' in reopened
            run([str(session), '--device', other, '--run-for', '1'],
                f"doesn't match the session's device {model}", ok=False)
    print('zodiac-1-boot: 32 MiB and active LCD; both models save, reopen and reject a cross-model launch')


if __name__ == '__main__':
    main()
