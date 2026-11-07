#!/usr/bin/env python3
"""Firmware-free checks for the measured E2 de-NVFS setup override."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot as boot


def blank(height=320):
    return [bytearray([255, 255, 255, 255]) * 320 for _ in range(height)]


def paint(rows, left, top, right, bottom, colour):
    for y in range(top, bottom):
        for x in range(left, right):
            rows[y][4*x:4*x+3] = bytes(colour)


class DenvfsBootTest(unittest.TestCase):
    def test_variant_routes_only_the_qualified_e2_override(self):
        self.assertEqual(boot.variant_recipe('palm-e2'), 'e2')
        self.assertIs(boot.launcher_check('palm-e2'), boot.upper_wallpaper_launcher)
        with patch.dict(boot.SETUP_RECIPES, {'e2-denvfs': Mock()}) as recipes:
            boot.setup_device('palm-e2', 'core', ROOT, {'id': 'e2-denvfs'})
            recipes['e2-denvfs'].assert_called_once_with('core', ROOT)
        self.assertIs(boot.launcher_check('palm-e2', 'e2-denvfs'), boot.e2_denvfs_launcher)
        for device, variant in [('palm-tx', 'e2-denvfs'), ('palm-e2', 'unknown')]:
            with self.subTest(device=device, variant=variant), self.assertRaises(ValueError):
                boot.setup_device(device, Mock(), ROOT, variant)

    def test_black_target_requires_triangles_and_white_gaps(self):
        self.assertFalse(boot.e2_denvfs_target(None, 20, 20))
        rows = blank()
        self.assertFalse(boot.e2_denvfs_target(rows, 20, 20))
        for x in (10, 30):
            for y in (10, 30):
                paint(rows, x, y, x+1, y+1, (0, 0, 0))
        self.assertTrue(boot.e2_denvfs_target(rows, 20, 20))
        paint(rows, 10, 20, 11, 21, (0, 0, 0))
        self.assertFalse(boot.e2_denvfs_target(rows, 20, 20))
        self.assertFalse(boot.e2_denvfs_target(blank(480), 20, 20))

    def test_launcher_needs_purple_status_category_and_icons(self):
        rows = blank()
        self.assertFalse(boot.e2_denvfs_launcher(None))
        self.assertFalse(boot.e2_denvfs_launcher(rows))
        paint(rows, 0, 26, 312, 27, (49, 0, 156))
        paint(rows, 280, 6, 285, 20, (0, 0, 0))
        self.assertFalse(boot.e2_denvfs_launcher(rows))
        for left in (16, 120, 220):
            for top in (40, 110, 182, 254):
                paint(rows, left, top, left+20, top+20, (0, 96, 192))
        self.assertTrue(boot.e2_denvfs_launcher(rows))
        self.assertFalse(boot.e2_denvfs_launcher(rows + blank(160)))

    def test_preferences_matches_the_measured_boundary_counts(self):
        rows = blank()
        paint(rows, 10, 12, 60, 13, (49, 0, 156))
        paint(rows, 0, 60, 300, 61, (156, 156, 156))
        self.assertTrue(boot.e2_denvfs_prefs(rows))
        self.assertFalse(boot.e2_denvfs_launcher(rows))

    def test_soft_reset_date_time_uses_physical_home(self):
        rows = blank()
        paint(rows, 100, 12, 128, 13, (49, 0, 156))
        paint(rows, 200, 6, 236, 20, (0, 0, 0))
        core = Mock()
        def wait(path, check, timeout):
            if path.name == 'boot-frame.png':
                self.assertTrue(check(rows))
                return rows
            self.assertEqual(path.name, 'launcher.png')
            self.assertIs(check, boot.e2_denvfs_launcher)
        core.wait_frame.side_effect = wait
        boot.e2_denvfs_setup(core, ROOT)
        core.tap.assert_called_once_with(ROOT, 27, 352)

    def test_week_marker_rejects_popup_and_wrong_geometry(self):
        rows = blank()
        paint(rows, 0, 68, 320, 69, (0, 0, 0))
        paint(rows, 183, 204, 184, 205, (0, 0, 0))
        self.assertTrue(boot.e2_denvfs_week_start(rows, False))
        self.assertFalse(boot.e2_denvfs_week_start(rows, True))
        paint(rows, 183, 204, 184, 205, (255, 255, 255))
        paint(rows, 178, 204, 179, 205, (0, 0, 0))
        self.assertTrue(boot.e2_denvfs_week_start(rows, True))
        paint(rows, 145, 222, 242, 240, (0, 0, 0))
        self.assertFalse(boot.e2_denvfs_week_start(rows, True))
        self.assertFalse(boot.e2_denvfs_formats(rows + blank(160)))


if __name__ == '__main__':
    unittest.main()
