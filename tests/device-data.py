#!/usr/bin/env python3
"""Device inventory export and frontend recipe regressions, without private firmware."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot

spec = importlib.util.spec_from_file_location('generate_device_data', ROOT / 'tools/generate-device-data.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class DeviceDataTest(unittest.TestCase):
    def test_new_available_profile_reaches_both_exports(self):
        inventory = json.loads((ROOT / 'tools/device-profiles.json').read_text())
        added = copy.deepcopy(inventory['profiles'][1])
        added.update(name='test-fixture', display_name='Fixture', nor_bytes=123456, hal_id=0x11223344)
        inventory['profiles'].append(added)
        outputs = generator.outputs(inventory)
        exported = json.loads(next(data for path, data in outputs.items() if path.suffix == '.json'))
        self.assertEqual(exported['profiles'][-1]['hal_id'], 0x11223344)
        kotlin = next(data for path, data in outputs.items() if path.suffix == '.kt')
        self.assertIn('id = "test-fixture"', kotlin)
        self.assertIn('norSize = 123456L', kotlin)
        added['available'] = False
        kotlin = next(data for path, data in generator.outputs(inventory).items() if path.suffix == '.kt')
        self.assertNotIn('test-fixture', kotlin)

    def test_sd_bounds(self):
        profiles = {p['name']: p for p in json.loads((ROOT / 'tools/device-profiles.json').read_text())['profiles']}
        mib = 1 << 20
        self.assertEqual(profiles['palm-tx']['sd_max_bytes'], 2048 * mib)
        self.assertEqual(profiles['palm-zire-72']['sd_max_bytes'], 2048 * mib)
        self.assertEqual(profiles['palm-e2']['sd_max_bytes'], 1024 * mib)
        self.assertEqual(profiles['palm-e2']['sd_min_bytes'], 512)
        kotlin = (ROOT / 'android/app/src/main/kotlin/com/rm/frond/DeviceInventory.kt').read_text()
        self.assertIn('sdMax = 2147483648L', kotlin)
        self.assertIn('sdMax = 1073741824L', kotlin)

    def test_e3_generated_capabilities_and_geometry(self):
        profiles = {p['name']: p for p in json.loads((ROOT / 'tools/device-profiles.json').read_text())['profiles']}
        e3 = profiles['palm-e2-franken']
        self.assertTrue(e3['available'])
        self.assertEqual(e3['display_name'], 'Tungsten E3')
        self.assertEqual((e3['width'], e3['height'], e3['silkscreen_height']), (320, 480, 0))
        self.assertEqual(e3['setup'], 'e3')
        self.assertEqual(e3['launcher'], 'e3-stock')
        self.assertEqual(e3['nor_bytes'], 8 << 20)
        self.assertEqual(e3['raw_nand_bytes'], 34603008)
        self.assertFalse(e3['explicit_raw_nand'])
        self.assertEqual(e3['fast_audio_md5'], 'a79f98bddd50c61e7eb89eee46eb81bd')
        self.assertNotEqual(e3['fast_audio_md5'], profiles['palm-e2']['fast_audio_md5'])
        self.assertNotIn('power', e3['buttons'].split('|'))
        for key in ('sound', 'serial', 'usb', 'keyboard', 'infrared', 'bluetooth', 'hot_sync_button'):
            self.assertTrue(e3[key], key)
        for name in ('palm-tx', 'palm-e2', 'palm-iiix'):
            self.assertTrue(profiles[name]['hot_sync_button'], name)
        for name in ('palm-zire-72', 'sony-clie-tg50', 'repalm'):
            self.assertFalse(profiles[name]['hot_sync_button'], name)
        kotlin = (ROOT / 'android/app/src/main/kotlin/com/rm/frond/DeviceInventory.kt').read_text()
        self.assertIn('id = "palm-e2-franken"', kotlin)
        self.assertIn('hotSyncButton = true', kotlin)

    def test_memory_stick_sizes(self):
        profiles = {p['name']: p for p in json.loads((ROOT / 'tools/device-profiles.json').read_text())['profiles']}
        self.assertEqual(profiles['sony-clie-tg50']['ms_sizes_mb'], [4, 8, 16, 32, 64, 128])
        self.assertEqual(profiles['palm-tx']['ms_sizes_mb'], [])
        kotlin = (ROOT / 'android/app/src/main/kotlin/com/rm/frond/DeviceInventory.kt').read_text()
        self.assertIn('msSizes = listOf(4, 8, 16, 32, 64, 128)', kotlin)

    def test_available_profiles_have_measured_recipes(self):
        self.assertEqual(list(palm_boot.DEVICES), ['palm-tx', 'palm-e2', 'palm-e2-franken', 'repalm', 'palm-zire-72', 'sony-clie-tg50'])
        for name, p in palm_boot.DEVICES.items():
            if not p['available']:
                continue
            self.assertIn(p['setup'], palm_boot.SETUP_RECIPES, name)
            self.assertIn(p['launcher'], palm_boot.LAUNCHER_CHECKS, name)
            self.assertEqual(p['input_height'], p['height'] + p['silkscreen_height'])
            self.assertEqual(len(p['app_names'].split('|')), 4)
            self.assertEqual(len(set(p['buttons'].split('|'))), len(p['buttons'].split('|')))
        self.assertIn('sony-clie-tg50', palm_boot.DEVICES)

    def test_socket_runners_use_frontend_inventory(self):
        spec = importlib.util.spec_from_file_location('frond_run_inventory', ROOT / 'tools/frond-run.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.assertEqual(list(runner.DEVICES), ['palm-tx', 'palm-e2', 'palm-e2-franken', 'palm-zire-72', 'sony-clie-tg50'])
        choices = next(action.choices for action in runner.parser()._actions if action.dest == 'device')
        self.assertEqual(list(choices), [name for name in palm_boot.FRONTEND_DEVICES if name not in palm_boot.DEVICES_68K])
        self.assertIn('palm-iiix', palm_boot.FRONTEND_DEVICES)
        self.assertNotIn('palm-iiix', runner.DEVICES)
        for name in ('repalm',):
            self.assertIn(name, palm_boot.DEVICES)
            self.assertNotIn(name, runner.DEVICES)
        spec = importlib.util.spec_from_file_location('compatibility_inventory', ROOT / 'tools/app-compatibility.py')
        matrix = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matrix)
        self.assertEqual(matrix.runner.DEVICES, runner.DEVICES)

    def test_install_refresh_uses_available_arm_profiles_and_home_controls(self):
        spec = importlib.util.spec_from_file_location('frond_arm_install_inventory', ROOT / 'tests/frond-arm-install.py')
        refresh = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(refresh)
        self.assertEqual(refresh.DEVICES, tuple(name for name in palm_boot.FRONTEND_DEVICES if name in palm_boot.DEVICES))
        self.assertIn('palm-e2-franken', refresh.DEVICES)
        self.assertNotIn('palm-iiix', refresh.DEVICES)
        for name in ('palm-e2', 'palm-e2-franken', 'palm-zire-72', 'sony-clie-tg50'):
            core = Mock()
            device = refresh.Device(name)
            device.reopen(core, ROOT)
            if device.profile['home_key']:
                core.key.assert_called_once_with(ROOT, 'home-key')
            else:
                core.tap.assert_called_once_with(ROOT, device.profile['home_x'], device.profile['home_y'])

    def test_staged_runner_is_independent_of_frontend(self):
        staged = dict(palm_boot.PROFILES[0], name='staged-fixture', available=False,
                      runner_eligible=True, sound=False)
        inventory = {'profiles': [staged]}
        kotlin = next(data for path, data in generator.outputs(inventory).items() if path.suffix == '.kt')
        self.assertNotIn('staged-fixture', kotlin)
        self.assertIn('staged-fixture', palm_boot.runner_devices([staged]))
        self.assertNotIn('staged-fixture', palm_boot.frontend_devices([staged]))
        self.assertIn('staged-fixture', palm_boot.frontend_devices([dict(staged, available=True)]))
        self.assertNotIn('staged-fixture', palm_boot.runner_devices([dict(staged, runner_eligible=False)]))
        # Android runs an available DragonBall device in its 68k runner; its ROM has no one size.
        dragonball = dict(staged, name='dragonball-fixture', available=True, runs_68k=True, nor_bytes=None)
        kotlin = next(data for path, data in generator.outputs({'profiles': [dragonball]}).items() if path.suffix == '.kt')
        self.assertIn('dragonball-fixture', kotlin)
        self.assertIn('runs68k = true', kotlin)
        self.assertIn('norSize = 0L', kotlin)
        self.assertIn('dragonball-fixture', palm_boot.frontend_devices([dragonball]))
        for p in palm_boot.PROFILES:
            if p['device_type'] < 0:
                self.assertFalse(p['runner_eligible'])

    def test_e2_launcher_uses_measured_upper_wallpaper(self):
        blank = [bytes([255, 255, 255, 255]) * 320 for _ in range(320)]
        launcher = blank.copy()
        for y in range(40, 144):
            launcher[y] = bytes([100, 160, 220, 255]) * 320
        check = palm_boot.launcher_check('palm-e2')
        self.assertFalse(check(None))
        self.assertFalse(check(blank))
        self.assertTrue(check(launcher))
        self.assertFalse(palm_boot.launcher_check('palm-tx')(launcher))
        self.assertFalse(palm_boot.launcher_check('palm-zire-72')(launcher))

    def test_calibration_check_rejects_blank_frames(self):
        blank = [bytes([255, 255, 255, 255]) * 320 for _ in range(320)]
        self.assertFalse(palm_boot.calibration_target(None, 291, 292))
        self.assertFalse(palm_boot.calibration_target(blank, 291, 292))
        red = [bytearray(row) for row in blank]
        for x in range(282, 301):
            for y in range(283, 302):
                red[y][4*x:4*x+3] = bytes([220, 20, 20])
        self.assertTrue(palm_boot.calibration_target(red, 291, 292))
        self.assertFalse(palm_boot.calibration_target(red, 27, 27))

    def test_stock_launcher_needs_status_and_icon_grid(self):
        blank = [bytes([255, 255, 255, 255]) * 320 for _ in range(320)]
        self.assertFalse(palm_boot.stock_launcher(None))
        self.assertFalse(palm_boot.stock_launcher(blank))
        rows = [bytearray(row) for row in blank]
        for x in range(312):
            rows[26][4*x:4*x+3] = bytes([0, 96, 192])
        for y in range(6, 20):
            for x in range(300, 305):
                rows[y][4*x:4*x+3] = bytes([0, 0, 0])
        self.assertFalse(palm_boot.stock_launcher(rows))
        for left in (16, 120, 220):
            for top in (40, 110, 182, 254):
                for y in range(top, top+20):
                    for x in range(left, left+20):
                        rows[y][4*x:4*x+3] = bytes([0, 96, 192])
        self.assertTrue(palm_boot.stock_launcher(rows))
        self.assertTrue(palm_boot.launcher_check('palm-e2')(rows))

    def test_unqualified_recipe_sends_no_input(self):
        core = Mock()
        palm_boot.DEVICES['test-fixture'] = {'setup': 'unqualified'}
        try:
            with self.assertRaisesRegex(ValueError, 'No qualified setup recipe'):
                palm_boot.setup_device('test-fixture', core, ROOT)
            self.assertEqual(core.mock_calls, [])
        finally:
            del palm_boot.DEVICES['test-fixture']


if __name__ == '__main__':
    unittest.main()
