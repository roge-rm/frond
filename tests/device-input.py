#!/usr/bin/env python3
"""Reject synthetic device/cohort inputs before creating a SoC or SDL window."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'out/frond'


def header(hal=0x68737072, size=256, at=0):
    data = bytearray(size)
    struct.pack_into('<I', data, at + 8, 0xfeedbeef)
    struct.pack_into('<II', data, at + 0x80, 0x50616c6d, hal)
    return data


def e2_nor(size=8 << 20, at=0x20000):
    """E2 NOR layout: card header after the 128 KiB boot block."""
    return header(size=size, at=at)


ZIRE_SIZE = 14942208
AZ72, HSPR = 0x615a3732, 0x68737072


def zire_nor(size=ZIRE_SIZE, small=AZ72, big=AZ72, big_length=None, small_at=0, name=b'PalmCard'):
    """Zire 72 NOR layout: a small card at 0x0 and a big card at 0x20000 to the end."""
    data = bytearray(size)
    for at, hal, length in ((small_at, small, 0x10000),
                            (0x20000, big, ZIRE_SIZE - 0x20000 if big_length is None else big_length)):
        if at + 0x90 > size:
            continue
        struct.pack_into('<I', data, at + 8, 0xfeedbeef)
        data[at + 0x10:at + 0x10 + len(name) + 1] = name + b'\0'
        struct.pack_into('<III', data, at + 0x6c, length, 0, 0)
        struct.pack_into('<II', data, at + 0x80, 0x50616c6d, hal)
    return data


def session(path, device, *, nor=None, nand=0, display=6, ram=32 << 20, sd=False):
    nor = header() if nor is None else nor
    # A hand-built v6 envelope, independent of the C++ writer: the device is a 32-byte name field.
    memory, state, card = b'RAM', b'STATE', bytes(32 << 20) if sd else b''
    fields = [display, ram, 0, len(nor), nand, len(memory), len(state), len(card)]
    compressor = zlib.compressobj()
    with path.open('wb') as stream:
        stream.write(struct.pack('<II', 0x19800819, 6) + device.encode().ljust(32, b'\0')[:32])
        stream.write(struct.pack('<' + 'I' * len(fields), *fields))
        stream.write(compressor.compress(nor))
        block = bytes(1 << 20)
        remaining = nand
        while remaining:
            size = min(remaining, len(block))
            stream.write(compressor.compress(block[:size]))
            remaining -= size
        stream.write(compressor.compress(memory + state + card))
        stream.write(compressor.flush())
    return path


class DeviceInputTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='device-input-', dir=ROOT / 'build')
        cls.root = Path(cls.temp.name)
        cls.nor = cls.root / 'nor'
        cls.nand = cls.root / 'nand'
        cls.e2 = cls.root / 'e2'
        cls.e2.write_bytes(e2_nor())
        for file, size in ((cls.nor, 16 << 20), (cls.nand, 138412032)):
            with file.open('wb') as stream:
                stream.truncate(size)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def rejected(self, args, message):
        result = subprocess.run([str(CORE), *map(str, args)], capture_output=True, text=True,
                                timeout=10, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy',
                                                 'SDL_AUDIODRIVER': 'dummy'})
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertIn(message, result.stderr + result.stdout)
        self.assertNotIn('SDL', result.stderr)
        return result

    def test_window_scaling_options(self):
        for scale in ('1', '1.25', '1.5', '1.75', '2'):
            with self.subTest(scale=scale):
                self.rejected([self.nor, '--device', 'palm-tx', '--window-scale', scale,
                               '--smooth-lcd'], 'explicit --nand')
        for scale in ('0', '1.1', '2.25', 'nan', 'inf', '1.25oops'):
            with self.subTest(scale=scale):
                self.rejected([self.nor, '--window-scale', scale], 'invalid window scale')

    def test_fast_audio_requires_qualified_firmware(self):
        cases = ([self.nor, '--device', 'palm-tx', '--nand', self.nand], [self.e2])
        for args in cases:
            for option in ('--fast-audio', '--experimental-fast-audio'):
                with self.subTest(args=args, option=option):
                    self.rejected([*args, option],
                                  'requires a qualified device NOR image')

    def test_inventory(self):
        data = json.loads(subprocess.check_output([ROOT / 'out/frond-device-profiles']))
        self.assertEqual([p['name'] for p in data['profiles'] if p['status'] == 'tx-desktop'], ['palm-tx'])
        self.assertEqual([p['name'] for p in data['profiles'] if p['status'] == 'e2-desktop'], ['palm-e2'])
        self.assertEqual([p['name'] for p in data['profiles'] if p['status'] == 'zire72-desktop'], ['palm-zire-72'])
        self.assertEqual([p['name'] for p in data['profiles']],
                         ['palm-tx', 'palm-e2', 'palm-e2-franken', 'repalm',
                          'palm-zire-72', 'sony-clie-tg50', 'palm-iiix', 'alphasmart-dana', 'alphasmart-dana-wireless',
                          'palm-m515'])
        zire = data['profiles'][4]
        self.assertEqual((zire['cohort'], zire['status'], zire['nor_bytes'], zire['raw_nand_bytes']),
                         ('zire72-nor', 'zire72-desktop', ZIRE_SIZE, 0))
        self.assertEqual((zire['ram_min_bytes'], zire['ram_max_bytes'], zire['default_display_mode'],
                          zire['display_mode_mask']), (32 << 20, 32 << 20, 5, 1 << 5))
        self.assertEqual({p['cohort'] for p in data['profiles']},
                         {'tx-nor-nand', 'e2-nor-nand', 'patched-e2', 'paravirtual', 'zire72-nor', 'tg50-nor', 'reserved',
                          'ez68k-rom', 'vz68k-rom'})
        self.assertEqual(data['profiles'][2]['status'], 'experimental-desktop')
        self.assertEqual(data['profiles'][3]['status'], 'inherited-unverified')
        iiix = data['profiles'][6]
        self.assertEqual((iiix['name'], iiix['status'], iiix['runner_eligible'], iiix['runs_68k'], iiix['available']),
                         ('palm-iiix', 'experimental-desktop', False, True, True))
        self.assertTrue(all(p['status'] == 'reserved' for p in data['profiles'][9:]))

    def test_tx_raw_geometry_and_options(self):
        base = [self.nor, '--device', 'palm-tx']
        self.rejected(base, 'explicit --nand')
        self.rejected(base + ['--nand', self.e2], 'invalid NAND size')
        self.rejected([self.e2, '--device', 'palm-tx', '--nand', self.nand], 'does not match ROM')
        self.rejected(base + ['--nand', self.nand, '--ram-size', '16'], 'unsupported RAM size')
        self.rejected(base + ['--nand', self.nand, '--ram-size', '0'], 'unsupported RAM size')
        self.rejected(base + ['--nand', self.nand, '--ram-size', '4096'], 'overflows')
        self.rejected(base + ['--nand', self.nand, '--display', '320x320'], 'unsupported display mode')
        self.rejected([self.e2, '--nand', self.nand], 'invalid NAND size')

    def test_e2_nor_size_and_layout(self):
        cases = ((e2_nor(4 << 20), 'invalid NOR size; expected 8388608 bytes'),
                 (e2_nor((8 << 20) + 1), 'invalid NOR size; expected 8388608 bytes'),
                 (header(), 'invalid NOR size; expected 8388608 bytes'),
                 (e2_nor(at=0), 'invalid NOR layout'),
                 (e2_nor(at=0x40000), 'invalid NOR layout'))
        for index, (data, message) in enumerate(cases):
            with self.subTest(case=index):
                rom = self.root / f'e2-bad-{index}'
                rom.write_bytes(data)
                self.rejected([rom, '--run-for', '1'], message)
        # E2 sessions saved before these checks keep their NOR; the synthetic state then fails.
        for index, nor in enumerate((e2_nor(4 << 20), e2_nor(16 << 20), e2_nor(at=0))):
            with self.subTest(session=index):
                path = session(self.root / f'e2-old-session-{index}', 'palm-e2', nor=nor, nand=34603008,
                               display=5)
                result = self.rejected([path, '--run-for', '1'], 'failed to restore savestate')
                self.assertNotIn('invalid', result.stderr)

    def test_e2_denvfs_variant(self):
        def variant(size=6 << 20, at=0x20000):
            data = e2_nor(size, at)
            data[at + 0x10:at + 0x17] = b'rePalm\0'
            data[at + 0x30:at + 0x3a] = b'Dmitry.GR\0'
            struct.pack_into('<I', data, at + 0x6c, size - at)
            return data

        rom = self.root / 'e2-denvfs'
        rom.write_bytes(variant())
        # Valid alternate geometry reaches the independent NAND check, before SoC construction.
        self.rejected([rom, '--nand', self.e2], 'invalid NAND size')
        self.rejected([rom, '--device', 'palm-e2-franken'], 'does not match ROM')
        self.rejected([rom, '--fast-audio'], 'requires a qualified device NOR image')
        for field in (0x10, 0x16, 0x30, 0x39, 0x6c, 0x80, 0x84):
            with self.subTest(field=field):
                data = variant()
                data[0x20000 + field] ^= 1
                rom.write_bytes(data)
                message = 'unknown ROM device identity' if field in (0x80, 0x84) else 'invalid NOR size'
                self.rejected([rom, '--run-for', '1'], message)
        for size in ((6 << 20) - 1, (6 << 20) + 1):
            rom.write_bytes(variant(size))
            self.rejected([rom], 'invalid NOR size')
        for at in (0, 0x8000, 0x40000):
            rom.write_bytes(variant(at=at))
            self.rejected([rom], 'invalid NOR size')

    def test_e3_nor_identity_geometry_and_legacy_sessions(self):
        def e3(size=8 << 20, at=0x20000):
            return header(0x446d4752, size, at)

        rom = self.root / 'e3'
        for data, message in ((e3(4 << 20), 'invalid NOR size; expected 8388608 bytes'),
                              (e3((8 << 20) + 1), 'invalid NOR size; expected 8388608 bytes'),
                              (e3(at=0), 'invalid NOR layout'),
                              (e3(at=0x40000), 'invalid NOR layout')):
            rom.write_bytes(data)
            self.rejected([rom, '--run-for', '1'], message)
        rom.write_bytes(e3())
        self.rejected([rom, '--device', 'palm-e2'], 'does not match ROM')
        self.rejected([self.e2, '--device', 'palm-e2-franken'], 'does not match ROM')
        self.rejected([rom, '--display', '320x320'], 'unsupported display mode')
        self.rejected([rom, '--ram-size', '64'], 'unsupported RAM size')
        self.rejected([rom, '--fast-audio'], 'requires a qualified device NOR image')
        for index, nor in enumerate((e3(4 << 20), e3(16 << 20), e3(at=0))):
            path = session(self.root / f'e3-old-session-{index}', 'palm-e2-franken',
                           nor=nor, nand=34603008)
            result = self.rejected([path, '--desktop-socket', self.root / 'unused'],
                                   'failed to restore savestate')
            self.assertNotIn('invalid NOR', result.stderr)

    def test_inherited_cohorts_cannot_use_desktop(self):
        for hal in (0x50765076,):
            with self.subTest(hal=hal):
                rom = self.root / 'inherited'
                rom.write_bytes(header(hal))
                self.rejected([rom, '--desktop-socket', self.root / 'unused'], 'Palm TX, Tungsten E2 and Zire 72 only')

    def test_e2_replay_bounds_and_physical_app_buttons(self):
        replay = self.root / 'e2.replay'
        # A duration rejection proves parsing succeeded, without executing synthetic ROM.
        # Existing aliases keep their TX meaning; physical names avoid claiming an E2 Home key.
        for key in ('hard1', 'hard2', 'hard3', 'hard4', 'home', 'calendar', 'contacts', 'web',
                    'up', 'down', 'left', 'right', 'center', 'power', 'voice'):
            replay.write_text(f'0 key-down {key}\n1000 key-up {key}\n')
            self.rejected([self.e2, '--input-replay', replay, '--run-for', '1'],
                          'input replay extends beyond')
        replay.write_text('0 pen-down 319 439\n1000 pen-up\n')
        self.rejected([self.e2, '--input-replay', replay, '--run-for', '1'],
                      'input replay extends beyond')
        for x, y in ((320, 319), (319, 440), (-1, 0)):
            replay.write_text(f'0 pen-down {x} {y}\n')
            self.rejected([self.e2, '--input-replay', replay, '--run-for', '1'],
                          'invalid replay event')

    def test_unknown_and_mismatched_session_names(self):
        for device in ('palm-tx2', 'tungsten-e2', 'palm-m515'):
            with self.subTest(device=device):
                path = session(self.root / 'unknown', device)
                self.rejected([path], f"this session is for {device}, which Frond can't run")
        # A IIIx session goes to frond-68k.
        self.rejected([session(self.root / 'unknown', 'palm-iiix')], 'this session is for palm-iiix, which runs in frond-68k')
        for device in ('', 'Palm TX', 'palm_tx', 'x' * 32):
            with self.subTest(device=device):
                path = session(self.root / 'bad-name', device)
                self.rejected([path], 'invalid session image')
        path = session(self.root / 'mismatch', 'palm-e2-franken')
        self.rejected([path], 'does not match ROM')
        path = session(self.root / 'tx-tag-e2-rom', 'palm-tx')
        self.rejected([path], 'does not match ROM')
        path = session(self.root / 'e2', 'palm-e2')
        self.rejected([path, '--device', 'palm-tx'], "doesn't match the session's device palm-e2")

    def test_zire72_passes_validation_and_starts(self):
        rom = self.root / 'zire'
        data = zire_nor()
        struct.pack_into('<I', data, 0, 0xeafffffe)  # reset vector: b . so the board idles quietly
        rom.write_bytes(data)
        env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'}
        # Display and RAM defaults come from the profile (32 MiB, 320x320).
        for args in ([rom], [rom, '--device', 'palm-zire-72'], [rom, '--ram-size', '32', '--display', '320x320']):
            with self.subTest(args=args[1:]):
                result = subprocess.run([str(CORE), *map(str, args), '--no-sound', '--run-for', '1'],
                                        capture_output=True, text=True, timeout=30, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('bounded run complete; PC=0x0', result.stdout + result.stderr)
                self.assertNotIn('supported yet', result.stderr)
        # A Zire session whose NOR is the Zire's passes validation and fails only on its fake state.
        # The desktop's saved TX serial settings are dropped for non-TX devices before this point.
        path = session(self.root / 'zire-session', 'palm-zire-72', nor=zire_nor(), display=5)
        self.rejected([path], 'failed to restore savestate')

    def test_zire72_rejections(self):
        rom = self.root / 'zire-bad'

        def reject(data, message, *args):
            rom.write_bytes(data)
            result = self.rejected([rom, *map(str, args)], message)
            self.assertNotIn('supported yet', result.stderr)

        size = 'invalid NOR size; expected 14942208 bytes'
        for length in (ZIRE_SIZE - 1, ZIRE_SIZE - 0x10000, 8 << 20, 16 << 20, 0x20000, 0x10000):
            with self.subTest(size=length):
                reject(zire_nor(length), size)
        with self.subTest(case='zero length'):
            reject(b'', 'neither a supported ROM nor a session image')
        with self.subTest(case='tiny'):
            reject(b'\0' * 0x100, 'neither a supported ROM nor a session image')
        with self.subTest(case='both HALs E2'):
            reject(zire_nor(small=HSPR, big=HSPR), 'invalid NOR size; expected 8388608 bytes')
        with self.subTest(case='both HALs unknown'):
            reject(zire_nor(small=0x12345678, big=0x12345678), 'device: unknown')
        with self.subTest(case='big HAL differs'):
            reject(zire_nor(big=0x12345678), 'invalid NOR layout')
        with self.subTest(case='big HAL E2'):
            reject(zire_nor(big=HSPR), 'invalid NOR layout')
        with self.subTest(case='small HAL differs'):
            reject(zire_nor(small=0x12345678), 'device: unknown')
        with self.subTest(case='small signature damaged'):
            data = zire_nor()
            struct.pack_into('<I', data, 8, 0)
            reject(data, 'invalid NOR layout')
        with self.subTest(case='big size field'):
            for length in (0xe20001, 0xe1ffff, 0, 0xffffffff):
                reject(zire_nor(big_length=length), 'invalid NOR layout')
        with self.subTest(case='card names'):
            reject(zire_nor(name=b'OtherCard'), 'invalid NOR layout')
        good = zire_nor()
        with self.subTest(case='nand'):
            reject(good, 'invalid NAND size; expected 0 bytes', '--nand', self.e2)
        for ram in ('16', '64', '8'):
            with self.subTest(ram=ram):
                reject(good, 'unsupported RAM size', '--ram-size', ram)
        for display in ('320x480', '160x160'):
            with self.subTest(display=display):
                reject(good, 'unsupported display mode', '--display', display)
        with self.subTest(case='fast audio'):
            for option in ('--fast-audio', '--experimental-fast-audio'):
                reject(good, 'requires a qualified device NOR image', option)

    def test_zire72_device_and_session_mismatches(self):
        rom = self.root / 'zire-mismatch'
        rom.write_bytes(zire_nor())
        self.rejected([rom, '--device', 'palm-tx', '--nand', self.nand], 'does not match ROM')
        self.rejected([rom, '--device', 'palm-e2'], 'does not match ROM')
        e2 = self.root / 'e2-rom'
        e2.write_bytes(e2_nor())
        self.rejected([e2, '--device', 'palm-zire-72'], 'does not match ROM')
        self.rejected([self.nor, '--device', 'palm-zire-72'], 'neither a supported ROM nor a session image')
        cases = (('palm-zire-72', e2_nor()), ('palm-zire-72', header(0x50765076)),
                 ('palm-e2', zire_nor()), ('palm-tx', zire_nor()), ('palm-e2-franken', zire_nor()))
        for index, (device, nor) in enumerate(cases):
            with self.subTest(case=index):
                path = session(self.root / f'zire-mismatch-session-{index}', device, nor=nor,
                               nand=34603008 if device == 'palm-e2' else 0, display=5)
                self.rejected([path], 'selected device profile does not match ROM'
                              if device == 'palm-tx' else 'session device profile does not match ROM')
        path = session(self.root / 'zire-named', 'palm-zire-72', nor=zire_nor(), display=5)
        self.rejected([path, '--device', 'palm-e2'], "doesn't match the session's device palm-zire-72")
        # An unknown or damaged device name in a session is refused before any ROM is read.
        for device in ('palm-zire-7', 'palm-zire-72x', 'zire-72', 'zire72'):
            with self.subTest(device=device):
                path = session(self.root / 'zire-bad-name', device, nor=zire_nor(), display=5)
                self.rejected([path], f"this session is for {device}, which Frond can't run")

    def test_zire72_layout_in_a_session_and_other_devices_unchanged(self):
        # The layout check applies to sessions too.
        path = session(self.root / 'zire-bad-layout', 'palm-zire-72', nor=zire_nor(big_length=1), display=5)
        self.rejected([path], 'invalid NOR layout')
        path = session(self.root / 'zire-bad-size', 'palm-zire-72', nor=zire_nor(8 << 20), display=5)
        self.rejected([path], 'invalid NOR size; expected 14942208 bytes')
        path = session(self.root / 'zire-nand', 'palm-zire-72', nor=zire_nor(), nand=34603008, display=5)
        self.rejected([path], 'invalid NAND size; expected 0 bytes')
        # E2 and TX inputs are judged as before.
        self.rejected([self.nor], 'neither a supported ROM nor a session image')
        self.rejected([self.nor, '--device', 'palm-tx'], 'explicit --nand')

    def test_numbered_sessions_are_refused(self):
        # Sessions before v6 name the device by number; they are refused, never mapped.
        for version in (0, 1, 4, 5):
            with self.subTest(version=version):
                path = self.root / f'numbered-{version}'
                path.write_bytes(struct.pack('<III', 0x19800819, version, 3) + bytes(64))
                self.rejected([path], "saved by an older Frond and can't be loaded")

    def test_device_option_names(self):
        self.rejected([self.nor, '--device', 'tx'], 'unknown device profile tx')
        self.rejected([self.nor, '--device', '3'], 'unknown device profile 3')
        self.rejected([self.nor, '--device', 'sony-clie-tg50'], 'neither a supported ROM nor a session image')
        self.rejected([self.nor], 'neither a supported ROM nor a session image')
        self.rejected([self.nor, '--device', 'palm-e2'], 'neither a supported ROM nor a session image')
        self.rejected([self.e2, '--device', 'palm-e2', '--nand', self.nand], 'invalid NAND size')
        self.rejected([self.e2, '--device', 'palm-tx', '--nand', self.nand], 'does not match ROM')

    def test_session_ram_display_and_corruption(self):
        path = session(self.root / 'display', 'repalm', nor=header(0x50765076), display=99)
        self.rejected([path], 'unsupported display mode')
        path = session(self.root / 'ram', 'repalm', nor=header(0x50765076), ram=24 << 20)
        self.rejected([path], 'unsupported RAM size')
        path = session(self.root / 'broken', 'palm-e2')
        path.write_bytes(path.read_bytes()[:-1])
        self.rejected([path], 'invalid session image')

    def test_named_tx_sessions_load(self):
        # A palm-tx session decodes and routes to the TX, shown by a pre-SoC option rejection.
        for sd in (False, True):
            with self.subTest(sd=sd):
                path = session(self.root / 'tx', 'palm-tx', nor=bytes(16 << 20), nand=138412032, sd=sd)
                self.rejected([path, '--ram-size', '32'], 'cannot specify RAM size for an existing session')
                self.rejected([path, '--nand', self.nand], 'separate NAND image cannot be used')


if __name__ == '__main__':
    unittest.main()
