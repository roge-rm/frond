#!/usr/bin/env python3
"""Dana normal backend lifecycle and production malformed-state rollback, using private inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from palm_boot import Core, dana_setup, dana_launcher, dana_reset, session_file


def session_chunks(data):
    assert struct.unpack_from('<II', data) in ((0x19800819, 8), (0x19800819, 9))
    sizes = struct.unpack_from('<5Q', data, 48)
    assert struct.unpack_from('<I', data, 88)[0] == 0
    raw = zlib.decompress(data[92:])
    start = sum(sizes[:4])
    count = struct.unpack_from('<I', raw, start)[0]
    cursor = start + 4 + 8 * count
    chunks = {}
    for i in range(count):
        kind, words = struct.unpack_from('<II', raw, start + 4 + 8 * i)
        chunks[kind] = (cursor, words)
        cursor += 4 * words
    assert cursor == len(raw)
    assert struct.unpack_from('<I', raw, chunks[0x1e0][0])[0] in (5, 6, 7, 8, 9)
    return raw, chunks


def malformed(data):
    raw, chunks = session_chunks(data)
    cases = [('adc-bits', 0x140, 2, 32), ('st-version', 0x1e0, 0, 10),
             ('st-request', 0x1e0, 3, 9), ('st-reply', 0x1e0, 4, 4),
             ('st-keys', 0x1e0, 5, 4), ('st-packet', 0x1e0, 7, 3),
             ('st-main', 0x1e0, 11, 4096), ('st-supply', 0x1e0, 12, 4096),
             ('isp-word', 0x1f0, 6, 2), ('isp-dcword', 0x1f0, 17, 2),
             ('isp-buffer', 0x1f0, 20 + 0x2a, 4097)]
    if struct.unpack_from('<I', raw, chunks[0x1e0][0])[0] >= 6:
        cases += [('st-layout', 0x1e0, 0, 5), ('st-held-count', 0x1e0, 14, 9),
                  ('st-queue-head', 0x1e0, 15, 32), ('st-queue-count', 0x1e0, 16, 33),
                  ('st-scan', 0x1e0, 17, 255), ('st-cursor', 0x1e0, 7, 9)]
    if struct.unpack_from('<I', raw, chunks[0x1e0][0])[0] >= 7:
        cases += [('st-input-clock', 0x1e0, 287, 1), ('st-input-delay', 0x1e0, 285, 0xffffffff),
                  ('st-poll-index', 0x1e0, 288, 9), ('st-presented-scan', 0x1e0, 289, 255)]
    if struct.unpack_from('<I', raw, chunks[0x1e0][0])[0] >= 8:
        cases += [('st-sleep-cursor', 0x1e0, 297, 10), ('st-wake-mask0', 0x1e0, 298, 256),
                  ('st-wake-mask1', 0x1e0, 299, 256), ('st-sleep-state', 0x1e0, 300, 3)]
    for name, kind, word, value in cases:
        offset, words = chunks[kind]
        assert word < words
        bad = bytearray(raw)
        struct.pack_into('<I', bad, offset + 4 * word, value)
        yield name, data[:92] + zlib.compress(bad)
    if struct.unpack_from('<I', raw, chunks[0x1e0][0])[0] >= 6:
        bad = bytearray(raw)
        offset, words = chunks[0x1e0]
        for word in range(14, words):
            struct.pack_into('<I', bad, offset + 4 * word, 0)
        for word, value in ((7, 0), (8, 1), (16, 1), (25, 0x0f)):
            struct.pack_into('<I', bad, offset + 4 * word, value)
        yield 'st-queue-held-mismatch', data[:92] + zlib.compress(bad)


def run(rom, probe, root):
    inputs = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (rom, probe)}
    def open_core(image, log):
        return Core([image, '--device', 'alphasmart-dana'], root / log, '--speed', 'uncapped')
    def export(c, name):
        p = root / name
        c.send(f'db-export-db FrondSyncLog "{p}"', 'export written')
        return p.read_bytes()
    def disconnected(path):
        raw, chunks = session_chunks(path.read_bytes())
        at, words = chunks[0x1f0]
        values = struct.unpack_from(f'<{words}I', raw, at)
        # The guest enables OPR, but no enabled OHCI event is pending. The
        # unimplemented transfer-buffer sources stay disabled; DC sources are off.
        hc = values[20:]
        assert hc[0x25] == 0x10, hex(hc[0x25])
        assert hc[0x24] & hc[0x25] == 0
        assert hc[3] & hc[4] & 0x7f == 0
        assert values[15] == 0, hex(values[15])
        print(f'PASS disconnected HC hardware={hc[0x20]:04x}, sources={hc[0x25]:04x}, pending={hc[3]:08x}, DC enable={values[15]:08x}', flush=True)
    def install(c):
        reply = c.send(f'install "{probe}"', 'installation ')
        assert 'successful' in reply, reply
        reply = c.send('launch "Frond sync"', 'launch: ')
        assert 'started' in reply, reply
    c = open_core(rom, 'cold.log')
    try:
        dana_setup(c, root)
        c.wait_frame(root / 'cold-launcher.png', dana_launcher)
        c.send('audio-on', 'sound:')
        c.send('audio-off', 'sound:')
        for command in ('serial-mode on', 'hotsync'):
            c.send(command, 'unsupported:')
        inventory = json.loads((Path(__file__).resolve().parents[1] / 'tools/device-profiles.json').read_text())
        infrared = next(profile['infrared'] for profile in inventory['profiles'] if profile['name'] == 'alphasmart-dana')
        if infrared:
            c.send('irda-mode pty', 'infrared:')
            c.send('irda-mode off', 'infrared:')
        else:
            c.send('irda-mode on', 'unsupported:')
        install(c)
        original = export(c, 'original.pdb')
        c.send(f'save-session "{root / "saved.frond"}"', 'wrote session')
    finally:
        close_status = c.close()
        if sys.exc_info()[0] is None:
            assert close_status == 0, f"core exited {close_status}"
    disconnected(root / 'saved.frond')
    c = open_core(root / 'saved.frond', 'reload.log')
    try:
        assert export(c, 'reloaded.pdb') == original
        # Each malformed production chunk reaches EmSession::Load and board Load,
        # then restores the running whole-machine checkpoint.
        c.frame(root / 'before.png')
        before = (root / 'before.png').read_bytes()
        count = 0
        for name, data in malformed((root / 'saved.frond').read_bytes()):
            path = root / (name + '.frond')
            path.write_bytes(data)
            reply = c.send(f'load-session "{path}"', 'session refused:')
            assert 'machine restored' in reply, (name, reply)
            c.frame(root / (name + '.png'))
            assert (root / (name + '.png')).read_bytes() == before, name
            assert export(c, name + '.pdb') == original, name
            count += 1
        print(f'PASS {count} production malformed chunks and whole-session frame/database rollback', flush=True)
        dana_reset(c, root, False)
        assert 'started' in c.send('launch "Frond sync"', 'launch: ')
        export(c, 'soft-retained.pdb')
        dana_reset(c, root, True)
        reply = c.send('launch "Frond sync"', 'launch: ')
        assert 'started' not in reply, reply
        reply = c.send(f'db-export-db FrondSyncLog "{root / "erased.pdb"}"', 'export ')
        assert 'export failed: no database named FrondSyncLog' in reply, reply
        assert not (root / 'erased.pdb').exists(), reply
        install(c)
        post = export(c, 'post-hard.pdb')
        c.send(f'save-session "{root / "post-hard.frond"}"', 'wrote session')
    finally:
        close_status = c.close()
        if sys.exc_info()[0] is None:
            assert close_status == 0, f"core exited {close_status}"
    disconnected(root / 'post-hard.frond')
    c = open_core(root / 'post-hard.frond', 'post-hard-reload.log')
    try:
        assert export(c, 'post-hard-reloaded.pdb') == post
    finally:
        close_status = c.close()
        if sys.exc_info()[0] is None:
            assert close_status == 0, f"core exited {close_status}"
    assert all(hashlib.sha256(p.read_bytes()).hexdigest() == digest for p, digest in inputs.items())
    print('PASS cold setup/launcher, install/launch/export, soft reset, Power/Up hard reset, erased DB and fresh-process DB equality', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--evidence', type=Path, required=True)
    a = p.parse_args()
    a.evidence.mkdir(mode=0o700)
    run(a.rom.resolve(), a.probe.resolve(), a.evidence.resolve())
