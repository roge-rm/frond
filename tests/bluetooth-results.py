#!/usr/bin/env python3
"""Mutation checks for the independent Bluetooth Exchange Manager artifact checker."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import zipfile

spec = importlib.util.spec_from_file_location('bluetooth_results', Path(__file__).resolve().parents[1] / 'tools/check-bluetooth-probe.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def database(name, records):
    data = bytearray(78 + 8 * len(records) + 2)
    data[:len(name)] = name.encode('ascii')
    data[60:68] = b'DATAFrBO'
    struct.pack_into('>H', data, 76, len(records))
    for i, record in enumerate(records):
        struct.pack_into('>I', data, 78 + 8 * i, len(data))
        data.extend(record)
    return bytes(data)


def archive(path, raw):
    with zipfile.ZipFile(path, 'w') as output:
        output.writestr('arbitrary-export-name.pdb', raw)
    return path


def main():
    payload = bytes(((i * 73 + 17) ^ ((i >> 8) * 31)) & 255 for i in range(4096))
    assert hashlib.sha256(payload).hexdigest() == checker.PAYLOAD_SHA256
    registration = 'register err=0000 bytes=0'
    send = ['send-begin', 'put err=0000 bytes=0', 'send err=0000 bytes=4096',
            'disconnect err=0000 bytes=4096', 'send-complete']
    receive = ['receive-begin', 'accept err=0000 bytes=0', 'receive err=0000 bytes=4096',
               'receive-disconnect err=0000 bytes=4096', 'receive-complete']
    with tempfile.TemporaryDirectory(prefix='frond-bt-results-') as temporary:
        root = Path(temporary)
        def log(name, rows):
            return archive(root / name, database(checker.LOG_NAME, [row.encode() + b'\0' for row in rows]))
        tx = log('send.zip', [registration] + send)
        rx = log('receive.zip', [registration] + send + receive)
        raw = root / 'payload.bin'
        raw.write_bytes(payload)
        received = archive(root / 'payload.zip', database(checker.PAYLOAD_NAME, [payload]))
        assert checker.check('send', tx, raw)['status'] == 'pass'
        assert checker.check('receive', rx, received)['status'] == 'pass'
        assert checker.check('receive', rx, received, tx)['status'] == 'pass'
        def refused(role, current, body=received, before=None):
            try:
                checker.check(role, current, body, before)
            except (ValueError, UnicodeError, struct.error):
                return
            raise AssertionError('invalid artifact was accepted')
        refused('send', log('later-failure.zip', [registration] + send + ['send-begin', 'put err=1503 bytes=0']), raw)
        refused('send', log('duplicate.zip', [registration] + send + ['send-complete']), raw)
        refused('receive', log('disconnect-error.zip', [registration] + receive[:-2] + ['receive-disconnect err=1503 bytes=4096', 'receive-complete']))
        refused('receive', log('missing-disconnect.zip', [registration] + receive[:-2] + ['receive-complete']))
        refused('receive', log('registration-error.zip', [registration, 'register err=0001 bytes=0'] + receive))
        refused('receive', rx, received, rx)
        refused('receive', rx, received, log('changed-history.zip', ['altered history']))
        raw.write_bytes(payload[:2048] + bytes([payload[2048] ^ 1]) + payload[2049:])
        refused('send', tx, raw)
        wrong = bytearray(database(checker.LOG_NAME, [(registration + '\0').encode()] + [(row + '\0').encode() for row in send]))
        wrong[64:68] = b'FrXX'
        refused('send', archive(root / 'wrong-creator.zip', wrong), received)
        malformed = bytearray(database(checker.PAYLOAD_NAME, [payload]))
        struct.pack_into('>I', malformed, 78, 4)
        refused('receive', rx, archive(root / 'bad-offset.zip', malformed))
        refused('receive', rx, archive(root / 'extra-record.zip', database(checker.PAYLOAD_NAME, [payload, b'extra'])))
        bad_string = database(checker.LOG_NAME, [(registration + '\0').encode(), b'send-begin'])
        refused('send', archive(root / 'bad-string.zip', bad_string), received)
    print('PASS Bluetooth results: original API sequence, exact payload, target identity, fresh unchanged history and malformed/failed-attempt refusal')


if __name__ == '__main__':
    main()
