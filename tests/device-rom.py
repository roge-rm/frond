#!/usr/bin/env python3
"""Real-firmware device checks. Missing private inputs are reported as skips.

The Zire 72 also runs its existing rejection, setup, storage and peripheral suite,
including the FROND_ZIRE_* opt-ins. Other profiles take --nor and optional --nand;
--launcher adds measured setup and a fresh-process saved-session check.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import struct
import zlib
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# FROND_DEVICE_IMAGES is the firmware folder (deviceimages/palm).
sys.path.insert(0, str(ROOT / 'tools'))
from guest_process import run_guest
from palm_boot import CORE, CORE_68K, DEVICES_68K, PROFILES, ROM_DEVICES, Core, launcher_check, palm_images, setup_device, reset_recipe, session_file
ZIRE_NOR = palm_images() / 'Palm/Zire-72/Palm-Zire72-nor.bin'
from guest_faults import check_diagnostics, describe, parse_contract, observed_faults


def core_for(device):
    """The runner for a device; reads CORE at call time so the harness can swap it."""
    return CORE_68K if device in DEVICES_68K else CORE



def sha(path):
    with session_file(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def synthetic_suite(args):
    """Malformed TG50 inputs reject before creating a guest machine."""
    profile = next(p for p in PROFILES if p['name'] == args.device)
    if profile['cohort'] != 'tg50-nor':
        raise ValueError('--synthetic-rejection requires the TG50 NOR geometry')

    class ProfileRejectionTest(unittest.TestCase):
        @classmethod
        def setUpClass(cls):
            cls.temp = tempfile.TemporaryDirectory(prefix='device-rejection-', dir=ROOT / 'build')
            cls.root = Path(cls.temp.name)
            cls.nor = bytearray(profile['nor_bytes'])
            for at in (profile['header_offset'], profile['big_card_offset']):
                struct.pack_into('<I', cls.nor, at + 8, 0xfeedbeef)
                cls.nor[at + 0x10:at + 0x19] = b'PalmCard\0'
                struct.pack_into('<I', cls.nor, at + 0x6c, len(cls.nor) - at)
                struct.pack_into('<II', cls.nor, at + 0x80, profile['company_id'], profile['hal_id'])

        @classmethod
        def tearDownClass(cls):
            cls.temp.cleanup()

        def reject(self, data, message, *options, selected=True):
            image = self.root / 'synthetic.bin'
            image.write_bytes(data)
            command = [str(CORE), str(image), '--no-sound', '--run-for', '1']
            if selected:
                command += ['--device', args.device]
            # These commands exit before SoC construction, so they do not acquire an emulator.
            result = subprocess.run(command + list(options), capture_output=True, text=True, timeout=20,
                                    env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn(message, output)
            self.assertNotIn('Invalid instr', output)
            if args.evidence:
                with (args.evidence / 'synthetic-rejections.log').open('a') as log:
                    log.write(output + '\n')

        def session(self, *, name=None, nor=None, ram=None, display=None, nand=b'', sd=b''):
            nor = self.nor if nor is None else nor
            name = (name or args.device).encode().ljust(32, b'\0')
            header = struct.pack('<II', 0x19800819, 6) + name
            toc = struct.pack('<8I', display or profile['default_display_mode'], ram or profile['ram_min_bytes'],
                              0, len(nor), len(nand), 1, 1, len(sd))
            return header + toc + zlib.compress(bytes(nor) + nand + b'\0\0' + sd)

        def test_board_gate(self):
            if profile['runner_eligible']:
                # A concrete board must never run valid-looking synthetic NOR bytes.
                self.reject(self.session(), 'TG50 session requires complete RAM and CPU/peripheral state')
                return
            self.reject(self.nor, 'native board is not implemented')
            self.reject(self.nor, 'native board is not implemented', selected=False)
            self.reject(self.session(), 'native board is not implemented')

        def test_geometry_and_layout(self):
            for size in (len(self.nor)-1, len(self.nor)+1, 16 << 20, 8 << 20):
                self.reject(self.nor[:size] + bytes(max(0, size-len(self.nor))), 'invalid NOR size')
            for at in (8, profile['big_card_offset']+8, profile['big_card_offset']+0x80,
                       profile['big_card_offset']+0x84, profile['big_card_offset']+0x6c):
                bad = bytearray(self.nor)
                bad[at] ^= 1
                self.reject(bad, 'invalid NOR layout')
            nand = self.root / 'nand.bin'
            nand.write_bytes(b'\0')
            self.reject(self.nor, 'invalid NAND size', '--nand', str(nand))

        def test_identity_and_inputs(self):
            for at in (0x80, 0x84):
                bad = bytearray(self.nor)
                bad[at] ^= 1
                self.reject(bad, 'unknown ROM device identity')
                self.reject(bad, 'unknown ROM device identity', '--device', 'palm-e2', selected=False)
                if at == 0x84:
                    self.reject(bad, 'unknown ROM device identity', '--device', 'palm-tx', selected=False)
            for name in ('palm-e2', 'palm-zire-72', 'palm-tx'):
                self.reject(self.nor, 'does not match ROM', '--device', name, selected=False)
            for ram in ('0', '8', '17', '32'):
                self.reject(self.nor, 'unsupported RAM size', '--ram-size', ram)
            for display in ('160x160', '320x480'):
                self.reject(self.nor, 'unsupported display mode', '--display', display)
            self.reject(self.nor, 'requires a qualified device NOR image', '--fast-audio')
            replay = self.root / 'outside-screen.replay'
            replay.write_text('0 pen-down 160 320\n')
            self.reject(self.nor, 'invalid replay event', '--input-replay', str(replay))

        def test_sessions(self):
            self.reject(self.session(name='palm-e2'), 'session device profile does not match ROM', selected=False)
            self.reject(self.session(), "doesn't match the session", '--device', 'palm-e2', selected=False)
            self.reject(self.session(ram=32 << 20), 'unsupported RAM size')
            self.reject(self.session(display=6), 'unsupported display mode')
            self.reject(self.session(nand=b'\0'), 'invalid NAND size')
            self.reject(self.session(sd=bytes(512)), 'session SD card is not supported')
            bad = bytearray(self.nor)
            bad[profile['big_card_offset']+8] ^= 1
            self.reject(self.session(nor=bad), 'invalid NOR layout')
            self.reject(self.session(nor=self.nor[:-1]), 'invalid NOR size')
            self.reject(self.session()[:-2], 'invalid session image')

    return unittest.defaultTestLoader.loadTestsFromTestCase(ProfileRejectionTest)


def suite(args):
    if args.device == 'palm-zire-72' and args.nor is None:
        if args.sha256 and ZIRE_NOR.is_file() and sha(ZIRE_NOR) != args.sha256:
            raise ValueError('NOR does not match the requested SHA-256')
        if args.launcher:
            os.environ['FROND_ZIRE_LAUNCHER'] = '1'
        if args.evidence:
            os.environ['FROND_ZIRE_EVIDENCE'] = str(args.evidence)
        spec = importlib.util.spec_from_file_location('zire72_rom', ROOT / 'tests/zire72-rom.py')
        legacy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(legacy)
        return unittest.defaultTestLoader.loadTestsFromModule(legacy)

    def session_args(path):
        return [path, '--device', args.device, '--engine', getattr(args, 'engine', 'exact')]

    @unittest.skipUnless(args.nor and args.nor.is_file() and core_for(args.device).is_file(),
                         'private NOR or built core missing; pass --nor')
    class DeviceRomTest(unittest.TestCase):
        @classmethod
        def setUpClass(cls):
            cls.inputs = [p for p in (args.nor, args.nand) if p is not None]
            cls.hashes = {str(p): sha(p) for p in cls.inputs}
            if args.sha256:
                assert cls.hashes[str(args.nor)] == args.sha256, 'NOR does not match the inventoried hash'
            cls.root = args.evidence or Path(tempfile.mkdtemp(prefix=f'device-rom-{args.device}-', dir=ROOT / 'build'))
            cls.root.mkdir(parents=True, exist_ok=True)
            (cls.root / 'inputs.json').write_text(json.dumps(cls.hashes, indent=2) + '\n')
            cls.contract = getattr(args, 'fault_contract', None)
            if getattr(args, 'expected_faults', None):
                cls.contract_hash = args.fault_contract_hash
                (cls.root / 'expected-faults.json').write_bytes(args.fault_contract_bytes)
                (cls.root / 'expected-faults-identity.json').write_text(json.dumps(
                    {'source': str(args.expected_faults), 'sha256': cls.contract_hash}, indent=2) + '\n')
            cls.image = ['--device', args.device, args.nor, '--engine', getattr(args, 'engine', 'exact')]
            if args.nand:
                cls.image += ['--nand', args.nand]

        @classmethod
        def tearDownClass(cls):
            after = {str(p): sha(p) for p in cls.inputs}
            (cls.root / 'inputs-after.json').write_text(json.dumps(after, indent=2) + '\n')
            assert after == cls.hashes, 'private firmware changed'


        def check_phase(self, phase, path):
            text = path.read_text()
            try:
                observed = describe(observed_faults(text))
                (self.root / f'faults-{phase}.json').write_text(json.dumps(observed, indent=2) + '\n')
                check_diagnostics(text, phase, self.contract)
            except ValueError as error:
                self.fail(str(error))

        def test_boot(self):
            try:
                result = run_guest([str(core_for(args.device)), *map(str, self.image), '--no-sound', '--run-for', '12'],
                                   capture_output=True, text=True, timeout=args.boot_timeout,
                                   env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
            except subprocess.TimeoutExpired as error:
                (self.root / 'boot.log').write_text((error.output or '') + (error.stderr or ''))
                raise
            (self.root / 'boot.log').write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(args.device, result.stderr)
            self.check_phase('boot', self.root / 'boot.log')

        @unittest.skipUnless(args.launcher, 'pass --launcher for setup and session acceptance')
        def test_launcher_and_fresh_process_session(self):
            session = self.root / 'launcher.session'
            core = Core(self.image, self.root / 'setup.log')
            try:
                setup_device(args.device, core, self.root)
                core.wait_frame(self.root / 'launcher.png', launcher_check(args.device), 60)
                core.send(f'save-session "{session}"', 'wrote session')
            finally:
                self.assertEqual(core.close(), 0)
            self.assertTrue(session_file(session).is_file())
            session_hash = sha(session)
            reloaded = Core(session_args(session), self.root / 'reload.log')
            try:
                reloaded.wait_frame(self.root / 'reloaded.png', launcher_check(args.device), 60)
            finally:
                self.assertEqual(reloaded.close(), 0)
            after_hash = sha(session)
            (self.root / 'session-identity.json').write_text(json.dumps(
                {'before_reload': session_hash, 'after_reload': after_hash}, indent=2) + '\n')
            self.assertEqual(after_hash, session_hash)
            errors = []
            for phase in ('setup', 'reload'):
                try:
                    self.check_phase(phase, self.root / (phase + '.log'))
                except self.failureException as error:
                    errors.append(str(error))
            self.assertFalse(errors, '\n'.join(errors))

        @unittest.skipUnless(getattr(args, 'resets', False), 'pass --resets for measured functional reset flows')
        def test_reset_hard(self):
            self.run_reset(True)

        @unittest.skipUnless(getattr(args, 'resets', False), 'pass --resets for measured functional reset flows')
        def test_reset_soft(self):
            self.run_reset(False)

        def run_reset(self, hard):
            phase = 'reset-hard' if hard else 'reset-soft'
            root = self.root / phase
            root.mkdir()
            session = self.root / 'launcher.session'
            before_hash = sha(session)
            core = Core(session_args(session), root / 'guest.log')
            try:
                core.wait_frame(root / 'before.png', launcher_check(args.device), 60)
                reset_recipe(args.device)(core, root, hard)
                core.send(f'save-session "{root / "continued.session"}"', 'wrote session')
            finally:
                self.assertEqual(core.close(), 0)
            after_hash = sha(session)
            (root / 'session-identity.json').write_text(json.dumps(
                {'input_before': before_hash, 'input_after': after_hash,
                 'continued': sha(root / 'continued.session')}, indent=2) + '\n')
            self.assertEqual(after_hash, before_hash)
            self.check_phase(phase, root / 'guest.log')

        @unittest.skipUnless(args.launcher and args.device in DEVICES_68K, 'pass --launcher for a 68k device')
        def test_running_session_refusal_68k(self):
            # Damaged sessions loaded into a running frond-68k are refused and leave it running as it was.
            root = self.root / 'refusal'
            root.mkdir()
            good = session_file(self.root / 'launcher.session').read_bytes()
            damaged = {'truncated': good[:-2], 'garbage': bytes(len(good)), 'empty': b''}
            # A session for another device: the same file with the name field changed.
            other = bytearray(good)
            other[8:40] = b'palm-tx'.ljust(32, b'\0')
            damaged['other-device'] = bytes(other)
            core = Core(session_args(self.root / 'launcher.session'), root / 'guest.log')
            try:
                core.wait_frame(root / 'before.png', launcher_check(args.device), 60)
                for name, data in damaged.items():
                    path = root / f'{name}.session'
                    path.write_bytes(data)
                    line = core.send(f'load-session "{path}"', ('session refused', 'loaded session'))
                    self.assertIn('machine unchanged', line, name)
                core.wait_frame(root / 'after.png', launcher_check(args.device), 30)
                core.key(root, 'hard1', hold_ms=300)
                core.wait_frame(root / 'input.png', lambda rows: not launcher_check(args.device)(rows), 30)
            finally:
                self.assertEqual(core.close(), 0)
            self.check_phase('refusal', root / 'guest.log')

        @unittest.skipUnless(args.launcher and args.device in DEVICES_68K, 'pass --launcher for a 68k device')
        def test_saved_contrast_68k(self):
            # Contrast changes the LCD live and stays with the saved session.
            root = self.root / 'contrast'
            root.mkdir()

            def panel(rows):
                return max(row[x] for row in rows for x in range(0, len(row), 4))  # Red of the unlit panel.
            core = Core(session_args(self.root / 'launcher.session'), root / 'guest.log')
            try:
                normal = panel(core.wait_frame(root / 'normal.png', launcher_check(args.device), 60))
                core.send('display contrast=90', 'display:')
                dark = panel(core.wait_frame(root / 'dark.png', lambda rows: rows and panel(rows) < normal - 20, 30))
                core.send('window-snapshot "{}"'.format(root / 'window.png'), 'window snapshot')
                core.send(f'save-session "{root / "dark.session"}"', 'wrote session')
            finally:
                self.assertEqual(core.close(), 0)
            core = Core(session_args(root / 'dark.session'), root / 'reload.log')
            try:
                rows = core.wait_frame(root / 'reloaded.png', launcher_check(args.device), 60)
                self.assertEqual(panel(rows), dark)
            finally:
                self.assertEqual(core.close(), 0)

        @unittest.skipUnless(args.launcher and args.device not in DEVICES_68K and
                             (ROOT / 'build/test-device-session').is_file(),
                             'pass --launcher and build device-session-probe for running-state rollback')
        def test_running_session_rollback(self):
            result = run_guest([str(ROOT / 'build/test-device-session'), str(session_file(self.root / 'launcher.session'))],
                               capture_output=True, text=True, timeout=args.boot_timeout)
            (self.root / 'rollback.log').write_text(result.stdout + result.stderr)
            if result.returncode == 77:
                if self.contract is not None:
                    self.fail('explicit fault qualification requires supported running-state rollback')
                self.skipTest('running-state rollback probe supports PXA boards')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('continued guest input', result.stdout)
            self.check_phase('rollback', self.root / 'rollback.log')

    return unittest.defaultTestLoader.loadTestsFromTestCase(DeviceRomTest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, choices=[p['name'] for p in PROFILES if p['device_type'] >= 0])
    parser.add_argument('--synthetic-rejection', action='store_true', help='test TG50 malformed inputs without private firmware')
    parser.add_argument('--nor', '--rom', dest='nor', type=Path, help='NOR, or the ROM of a 68k device')
    parser.add_argument('--nand', type=Path)
    parser.add_argument('--sha256', help='expected NOR SHA-256 from the private inventory')
    parser.add_argument('--engine', choices=('exact', 'fast'), default='exact')
    parser.add_argument('--launcher', action='store_true')
    parser.add_argument('--resets', action='store_true', help="run the profile recipe's measured functional resets")
    parser.add_argument('--expected-faults', type=Path,
                        help='private JSON of observed fault signatures/counts, requires --launcher, --resets and --sha256')
    parser.add_argument('--boot-timeout', type=float, default=300, help='boot deadline in seconds, including waiting for a slot')
    parser.add_argument('--evidence', type=Path, help='new directory, existing evidence is refused')
    args = parser.parse_args()
    if args.device in DEVICES_68K and args.engine == 'fast':
        parser.error('the Fast engine is not available for 68k devices')
    if args.boot_timeout <= 0:
        parser.error('--boot-timeout must be positive')
    if args.evidence:
        try:
            args.evidence.mkdir(parents=True, exist_ok=False)
        except FileExistsError:
            parser.error('Evidence directory already exists: ' + str(args.evidence))
    try:
        if args.resets:
            if not args.launcher:
                raise ValueError('--resets requires --launcher')
            reset_recipe(args.device)
        if args.expected_faults:
            if args.synthetic_rejection or not args.nor or not args.nor.is_file() or not core_for(args.device).is_file():
                raise ValueError('expected-faults qualification requires the original NOR and built core')
            if not (ROOT / 'build/test-device-session').is_file():
                raise ValueError('build device-session-probe before expected-faults qualification')
            args.fault_contract_bytes = args.expected_faults.read_bytes()
            args.fault_contract_hash = hashlib.sha256(args.fault_contract_bytes).hexdigest()
            args.fault_contract = parse_contract(args.fault_contract_bytes, device=args.device,
                actual_sha256=sha(args.nor), pinned_sha256=args.sha256,
                launcher=args.launcher, resets=args.resets)
        if args.synthetic_rejection:
            selected = synthetic_suite(args)
        else:
            if args.device not in ROM_DEVICES:
                raise ValueError('native board is not implemented for ' + args.device)
            selected = suite(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    result = unittest.TextTestRunner(verbosity=2).run(selected)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
