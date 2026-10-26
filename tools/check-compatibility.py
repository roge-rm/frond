#!/usr/bin/env python3
"""Run the TX compatibility matrix; firmware-backed rows are optional and private."""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import sys
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


results = module('compatibility_results', ROOT / 'tools/compatibility-results.py')
runner = module('frond_run', ROOT / 'tools/frond-run.py')


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path, help='new private result directory')
    p.add_argument('--core-test', type=Path, default=ROOT / 'build/test-compatibility-core')
    p.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    p.add_argument('--session', type=Path, help='optional user-supplied ready TX session')
    p.add_argument('--probe', type=Path, default=ROOT / 'build/compatibility-probe/FrondCompatProbe.prc')
    p.add_argument('--timeout', type=runner.positive, default=300, help='wall seconds per firmware run, including slot wait')
    return p


def run(args):
    report = {'version': 1, 'status': 'error', 'rows': [], 'error': None,
              'started_at': datetime.now(timezone.utc).isoformat(),
              'hardware_comparison': 'deferred: physical TX unavailable'}
    root = None
    started = time.monotonic()
    try:
        output = args.output.resolve()
        if output.exists():
            raise ValueError('Use a new output directory')
        output.mkdir(parents=True, mode=0o700)
        root = output
        # This executable uses synthetic memory and never boots Palm firmware.
        with (root / 'synthetic.log').open('wb') as log:
            completed = subprocess.run([str(args.core_test.resolve())], stdout=log,
                                       stderr=subprocess.STDOUT, timeout=30)
        if completed.returncode:
            raise ValueError(f'Synthetic interpreter tests exited {completed.returncode}; see synthetic.log')
        report['synthetic_binary'] = {'path': str(args.core_test.resolve()), 'sha256': runner.sha256(args.core_test)}
        report['rows'].append({'case': 'pace-execution/native-interworking', 'status': 'pass',
                               'evidence': str(root / 'synthetic.log'), 'level': 'synthetic'})
        if not args.session:
            report['rows'].extend({'case': case, 'status': 'skipped', 'level': 'firmware',
                                   'reason': 'No user-supplied session'} for case in results.CASES)
        else:
            session, previous = args.session.resolve(), ''
            for label in ('initial', 'restored'):
                token = uuid.uuid4().hex
                config = root / (label + '-config.pdb')
                config.write_bytes(results.config_database(token, previous)); config.chmod(0o600)
                argv = ['--core', str(args.core.resolve()), '--session', str(session),
                        '--output', str(root / label), '--app', results.APP,
                        '--install', str(config), '--export-db', results.LOG,
                        '--screenshot', '1000', '--run-for', '2', '--headless',
                        '--timeout', str(args.timeout), '--startup-timeout', str(args.timeout),
                        '--ready-timeout', '60']
                if not previous:
                    argv += ['--install', str(args.probe.resolve())]
                run_result = runner.run(runner.parser().parse_args(argv))
                report.setdefault('runs', []).append({'phase': label, 'token': token,
                                                      'result': str(root / label / 'result.json')})
                if run_result['status'] != 'ok':
                    raise ValueError(f'{label}: {run_result["stage"]}: {run_result["error"]}')
                archive = Path(run_result['artifacts']['exports'][0]['path'])
                checks = results.check_records(results.read_log(archive), token, resumed=bool(previous))
                report['rows'].extend(dict(check, level='firmware', phase=label, evidence=str(archive))
                                      for check in checks)
                session, previous = Path(run_result['artifacts']['session']), token
        report['status'] = 'pass'
    except KeyboardInterrupt:
        report['error'] = 'Interrupted'
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        report['error'] = str(error)
    report['elapsed_seconds'] = round(time.monotonic() - started, 3)
    if root:
        (root / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    report = run(parser().parse_args())
    print(json.dumps(report))
    return 0 if report['status'] == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())
