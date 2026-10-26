#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('metrics', ROOT / 'tools/analyze-gameplay.py')
metrics = importlib.util.module_from_spec(spec); spec.loader.exec_module(metrics)

class GameplayMetrics(unittest.TestCase):
    def test_unchanged_frames_and_pause_do_not_inflate_rate(self):
        frames = [(0, 0, 'a'), (10_000_000, 20_000_000, 'a'),
                  (30_000_000, 60_000_000, 'b'), (40_000_000, 80_000_000, 'b'),
                  (50_000_000, 100_000_000, 'c')]
        r = metrics.summarize_frames(frames, (20_000_000, 40_000_000))
        self.assertEqual(r['changed_frames_after_input'], 2)
        self.assertEqual(r['guest_changed_frame_intervals']['median_ms'], 20)
        self.assertEqual(r['wall_changed_frame_intervals']['median_ms'], 40)
        self.assertEqual(r['first_change_after_input'], {'guest_ms': 10, 'wall_ms': 20})

    def test_no_response_is_not_zero_latency(self):
        r = metrics.summarize_frames([(0, 0, 'a'), (100, 100, 'a')], (10, 10))
        self.assertIsNone(r['first_change_after_input'])
        self.assertIsNone(r['wall_changed_frame_intervals'])

    def test_frontend_excludes_session_save_gap(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'core.log').write_text(
                'GAME_FRAME 900 0 a\n'
                'GAME_INPUT 0 1000 100000 2\n'
                'GAME_RUN 1000 1100 100000 0.0001 30000000 0\n'
                'GAME_FRAME 1100 200000 b\n'
                'GAME_RUN 1800 1900 100000 0.0001 30000000 0\n'
                'GAME_FRAME 1900 1000000 c\n'
                'GAME_INPUT 1 2000 1000000 6\n'
                'GAME_RUN 9999900 10000000 100000 0.0001 30000000 1\n'
                'GAME_FRAME 10000000 2000000 d\n')
            result = metrics.load(root)
            self.assertEqual(result['changed_frames_after_input'], 2)
            self.assertEqual(result['wall_changed_frame_intervals']['max_ms'], .8)
            self.assertLess(result['measured_wall_seconds'], .01)
            self.assertEqual(result['audio_underruns_total'], 0)

    def test_comparison_rejects_different_guest_work(self):
        fields = ('version', 'mips', 'sound', 'palm_epoch', 'guest_ns', 'cycles', 'lcd_frames',
                  'changed_frames', 'pcm_frames', 'pcm_zero_frames', 'pcm_peak',
                  'frame_sequence_hash', 'pcm_hash')
        before = {'kind': 'fixed-guest', 'summary': dict.fromkeys(fields, 1)}
        before['summary']['cpu_seconds'] = 10
        after = {'kind': 'fixed-guest', 'summary': dict(before['summary'], cpu_seconds=8)}
        self.assertTrue(metrics.compare(before, after)['matching_workload'])
        self.assertAlmostEqual(metrics.compare(before, after)['cpu_reduction_percent'], 20)
        after['summary']['frame_sequence_hash'] = 2
        self.assertFalse(metrics.compare(before, after)['matching_workload'])
        self.assertEqual(metrics.compare(before, after)['mismatches'], ['frame_sequence_hash'])

    def test_replay_parser(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'test.replay'
            for script, valid in [
                ('# fixture\n0 pen-up\n10 key-down up\n10 snapshot "x y.png" # note\n', True),
                ('1 pen-down 319 479\n2 pen-up\n', True),
                ('10 pen-up\n0 pen-up\n', False), ('-1 pen-up\n', False),
                ('1001 pen-up\n', False), ('0 pen-down 320 479\n', False),
                ('0 key-down unknown\n', False), ('0 snapshot\n', False),
                ('0 pen-up extra\n', False), ('0 unknown\n', False),
            ]:
                with self.subTest(script=script):
                    path.write_text(script)
                    result = subprocess.run([str(ROOT / 'build/benchmark-replay'), '--check-replay', str(path), '1000'], capture_output=True)
                    self.assertEqual(result.returncode == 0, valid, result.stderr)

if __name__ == '__main__':
    unittest.main()
