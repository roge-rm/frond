#!/usr/bin/env python3
"""E3 frame and machine-folder checks without private firmware."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot as boot


def blank(height=480):
    return [bytearray([255, 255, 255, 255]) * 320 for _ in range(height)]


def paint(rows, left, top, right, bottom, colour):
    for y in range(top, bottom):
        for x in range(left, right):
            rows[y][4*x:4*x+3] = bytes(colour)


class E3BootTest(unittest.TestCase):
    def test_launcher_requires_full_panel_and_icon_grid(self):
        rows = blank()
        self.assertFalse(boot.e3_launcher(None))
        self.assertFalse(boot.e3_launcher(rows))
        paint(rows, 0, 26, 312, 27, (0, 96, 192))
        paint(rows, 300, 6, 305, 20, (0, 0, 0))
        self.assertFalse(boot.e3_launcher(rows))
        for left in (16, 120, 220):
            for top in (40, 110, 182, 254):
                paint(rows, left, top, left+20, top+20, (0, 96, 192))
        self.assertTrue(boot.e3_launcher(rows))
        self.assertFalse(boot.e3_launcher(rows[:320]))

    def test_week_marker_rejects_popup_and_wrong_geometry(self):
        rows = blank()
        paint(rows, 0, 68, 320, 69, (0, 0, 0))
        paint(rows, 183, 204, 184, 205, (0, 0, 0))
        self.assertTrue(boot.e3_week_start(rows, False))
        self.assertFalse(boot.e3_week_start(rows, True))
        paint(rows, 183, 204, 184, 205, (255, 255, 255))
        paint(rows, 178, 204, 179, 205, (0, 0, 0))
        self.assertTrue(boot.e3_week_start(rows, True))
        paint(rows, 145, 222, 242, 240, (0, 0, 0))
        self.assertFalse(boot.e3_week_start(rows, True))
        self.assertFalse(boot.e3_formats(rows[:320]))

    def test_session_file_accepts_native_and_android_names(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            machine = Path(temporary) / 'saved.session'
            machine.mkdir()
            native = machine / 'saved.session.frond'
            native.write_bytes(b'private fixture')
            self.assertEqual(boot.session_file(machine), native)
            self.assertEqual(boot.session_file(native), native)
            native.rename(machine / 'session.frond')
            self.assertEqual(boot.session_file(machine), machine / 'session.frond')


if __name__ == '__main__':
    unittest.main()
