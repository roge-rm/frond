#!/usr/bin/env python3
"""frond-core --card: output lines, exit codes and cancelling with SIGTERM or SIGINT."""

import os
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / 'out' / 'frond'


def card(*args, **kwargs):
    return subprocess.run([str(CORE), '--card', *map(str, args)], capture_output=True, text=True, timeout=300, **kwargs)


def lines(result):
    return result.stdout.splitlines()


def main():
    work = Path(tempfile.mkdtemp(prefix='card-tool-', dir=ROOT / 'build'))
    try:
        # Make: progress lines, then ok KIND BYTES.
        r = card('make', 'sd', 64, work / 'a.frondsd')
        out = lines(r)
        assert r.returncode == 0, r
        assert out[-1] == f'ok sd {(work / "a.frondsd").stat().st_size}', out
        progress = [l for l in out[:-1] if l.startswith('progress ')]
        assert progress and len(progress) == len(out) - 1, out
        assert progress[-1] == 'progress 67108864/67108864', progress[-1]
        r = card('make', 'ms', 8, work / 'a.frondms')
        assert r.returncode == 0 and lines(r)[-1].startswith('ok ms '), r.stdout
        r = card('make', 'mspro', 256, work / 'a.frondmsp')
        assert r.returncode == 0 and lines(r)[-1].startswith('ok mspro '), r.stdout

        # Export, import, export again.
        r = card('export', work / 'a.frondsd', work / 'a.img')
        assert r.returncode == 0 and lines(r)[-1] == 'ok image 67108864', r.stdout
        r = card('import', work / 'a.img', 'sd', work / 'b.frondsd')
        assert r.returncode == 0 and lines(r)[-1].startswith('ok sd '), r.stdout
        assert (work / 'a.frondsd').read_bytes() == (work / 'b.frondsd').read_bytes()
        r = card('export', work / 'a.frondms', work / 'a2.frondms')
        assert r.returncode == 0 and (work / 'a.frondms').read_bytes() == (work / 'a2.frondms').read_bytes()

        # Failures: error CODE MESSAGE, exit 1; bad use exits 2.
        r = card('make', 'sd', 100, work / 'c.frondsd')
        assert r.returncode == 1 and lines(r)[-1].startswith('error size '), (r.returncode, r.stdout)
        assert not (work / 'c.frondsd').exists()
        r = card('import', work / 'a.frondms', 'sd', work / 'c.frondsd')
        assert r.returncode == 1 and lines(r)[-1].startswith('error size '), r.stdout
        r = card('make', 'sd')
        assert r.returncode == 2 and lines(r)[-1].startswith('error usage '), (r.returncode, r.stdout)
        r = card()
        assert r.returncode == 2, r.returncode

        # SIGTERM and SIGINT cancel and leave no file.
        for sig in (signal.SIGTERM, signal.SIGINT):
            target = work / 'big.frondsd'
            proc = subprocess.Popen([str(CORE), '--card', 'make', 'sd', '1886', str(target)], stdout=subprocess.PIPE,
                                    text=True)
            first = proc.stdout.readline()
            assert first.startswith('progress '), first
            proc.send_signal(sig)
            rest = proc.stdout.read().splitlines()
            code = proc.wait(timeout=60)
            assert code == 1 and rest[-1].startswith('error cancelled'), (code, rest[-3:])
            assert not target.exists()
            leftovers = sorted(p.name for p in work.iterdir() if '.tmp' in p.name)
            assert not leftovers, leftovers
        print('card tool cli ok')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
