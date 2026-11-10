#!/usr/bin/env python3
"""Compare a private packet trace with one explicitly finished raw job."""
import argparse
import hashlib
import re
from pathlib import Path

CALL = re.compile(r'CALL port=(\d+) addr=(\d+) ep=(\d+) pid=(\d+) toggle=(\d+) count=(\d+) bytes=([0-9a-f]*)$')

def accepted(trace):
    data = bytearray()
    pending = None
    toggle = 0
    collecting = False
    begins = 0
    for line in trace.splitlines():
        if 'printer:' in line and 'collecting bytes=0' in line:
            begins += 1
            if begins != 1:
                raise ValueError('expected one job')
            collecting = True
        match = CALL.fullmatch(line)
        if match:
            pending = match.groups()
        elif line.startswith('REPLY '):
            if pending and collecting and line.startswith('REPLY result=0 '):
                port, addr, ep, pid, got, count, payload = pending
                if ep == '1' and pid == '1':
                    chunk = bytes.fromhex(payload)
                    if len(chunk) != int(count):
                        raise ValueError('packet length mismatch')
                    if int(got) == toggle:
                        data.extend(chunk)
                        toggle ^= 1
            pending = None
    if begins != 1 or not data:
        raise ValueError('missing job or accepted data')
    return bytes(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('raw', type=Path)
    args = parser.parse_args()
    expected = accepted(args.trace.read_text())
    actual = args.raw.read_bytes()
    if actual != expected:
        raise SystemExit('raw differs from ACK-accepted nonduplicate bulk bytes')
    if args.raw.stat().st_mode & 0o777 != 0o600:
        raise SystemExit('raw permissions differ from 0600')
    print(f'PASS {len(actual)} bytes SHA256 {hashlib.sha256(actual).hexdigest()}')

if __name__ == '__main__':
    main()
