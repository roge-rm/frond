#!/usr/bin/env python3
"""Exercise slot 2 card swaps through the native runner with a made-up ROM."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import smoke

ROOT = Path(__file__).resolve().parents[1]


def paused_session(folder, root, name, *flags):
    work = root / name
    work.mkdir()
    extra = ['--engine', 'exact', '--card-swap', '--input-replay', root / 'pause.steps', *flags]
    try:
        return smoke.Run('tapwave-zodiac-2', work, folder, timeout=45, extra=extra)
    except Exception as error:
        raise AssertionError((work / 'core.log').read_text()) from error


def wait_for(run, command, expected, field='message'):
    deadline = time.monotonic() + 20
    while True:
        result = run.require(command)[field]
        if result == expected:
            return
        assert time.monotonic() < deadline, f'{command}: wanted {expected!r}, got {result!r}'
        time.sleep(0.1)


def main():
    with tempfile.TemporaryDirectory(prefix='zodiac-card-swap-', dir=ROOT / 'build') as directory:
        root = Path(directory)
        subprocess.run([sys.argv[1], root], check=True)
        folder = root / 'swap'
        (root / 'pause.steps').write_text('0 pause\n')

        # The changed identity makes this a different card, even with unchanged bytes.
        second = ['--sd2-card-identity', (b'3' * 32).hex()]
        first = ['--card-identity', (b'4' * 32).hex(), '--card-protected']
        cases = [
            (second, 'inserted', 'writable'),
            (first + second + ['--sd2-card-protected'], 'ejected', 'read-only'),
        ]
        for flags, first_before, second_protection in cases:
            run = paused_session(folder, root, first_before, *flags)
            try:
                wait_for(run, 'replay-state', 'paused', field='code')
                assert run.request('sd-state') == first_before
                assert run.request('sd2-state') == 'ejected'
                assert run.request('sd2-protection') == second_protection
                run.require('replay-continue')
                wait_for(run, 'sd2-state', 'inserted')
                wait_for(run, 'sd-state', 'inserted')
                assert run.request('sd-protection') == ('writable' if first_before == 'inserted' else 'read-only')
                assert ('card', 'sd2-inserted') in run.control.events
            finally:
                run.close()

        # The original frontend path can still supply the replacement over the control socket.
        sidecar = folder / 'sd2.frondsd'
        aside = root / sidecar.name
        sidecar.rename(aside)
        run = paused_session(folder, root, 'missing')
        try:
            assert run.request('sd2-state') == 'none'
            assert run.request('sd-state') == 'inserted'
            aside.rename(sidecar)
            run.require('sd2-put-in', second[1] + ' protected')
            assert run.request('sd2-state') == 'inserted'
            assert run.request('sd2-protection') == 'read-only'
        finally:
            run.close()
        print('PASS: native slot 2 swaps start ejected and put the right cards in independently')


if __name__ == '__main__':
    main()
