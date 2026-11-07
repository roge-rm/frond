#!/usr/bin/env python3
"""A session folder that doesn't load is refused with one line on stderr, not read as a file (patch 0260).
Before it, a folder with an sd.frondsd the session doesn't describe crashed the core at start with
std::bad_alloc. Cases: a folder holding a card file and an unreadable session (the message names the card file),
the same without a card file (a plain message), and, when a
palm-tx smoke session is there (FROND_ARM_SESSIONS/palm-tx or build/smoke/machines/palm-tx), the cardless
session with a card file beside it. FROND_CORE picks the core."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = Path(os.environ.get('FROND_CORE', ROOT / 'out/frond'))


def run_folder(folder, card):
    result = subprocess.run([str(CORE), str(folder), '--no-sound'], capture_output=True, text=True, timeout=60,
                            stdin=subprocess.DEVNULL, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy',
                                                           'SDL_AUDIODRIVER': 'dummy'})
    assert result.returncode == 1, (result.returncode, result.stderr)
    assert 'bad_alloc' not in result.stderr and 'terminate' not in result.stderr, result.stderr
    lines = [line for line in result.stderr.splitlines() if 'did not load' in line]
    assert len(lines) == 1 and str(folder) in lines[0], result.stderr
    assert ('card file' in lines[0]) == card, result.stderr


def main():
    work = Path(tempfile.mkdtemp(prefix='session-folder-card-', dir=ROOT / 'build'))
    try:
        broken = work / 'broken'
        broken.mkdir()
        (broken / 'working.frond').write_bytes(b'not a session')
        card = subprocess.run([str(CORE), '--card', 'make', 'sd', '32', str(broken / 'sd.frondsd')],
                              capture_output=True, text=True, timeout=120)
        assert card.returncode == 0, card
        run_folder(broken, True)
        plain = work / 'plain'
        plain.mkdir()
        (plain / 'working.frond').write_bytes(b'not a session')
        run_folder(plain, False)
        sessions = Path(os.environ.get('FROND_ARM_SESSIONS', ROOT / 'build/smoke/machines')) / 'palm-tx'
        session = next(iter(sorted(sessions.glob('*.frond'))), None) if sessions.is_dir() else None
        if session is None:
            print('cardless session case SKIPPED: no palm-tx session')
        else:
            folder = work / 'tx'
            folder.mkdir()
            shutil.copyfile(session, folder / 'working.frond')
            shutil.copyfile(broken / 'sd.frondsd', folder / 'sd.frondsd')
            run_folder(folder, True)
        print('ok')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
