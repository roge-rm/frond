#!/usr/bin/env python3
"""Run the TX compatibility matrix; firmware-backed rows are optional and private."""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLING = ('tools/check-compatibility.py', 'tools/compatibility-results.py',
           'tools/frond-run.py', 'tools/build-palm-probe.py',
           'tests/palm/compatibility-probe.c', 'tests/palm/compatibility-probe.rcp',
           'tests/palm/compatibility-probe.def', 'tests/palm/compatibility-native.c',
           'tests/palm/compatibility-thumb.S')


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
    origin = p.add_mutually_exclusive_group()
    origin.add_argument('--session', type=Path, help='optional user-supplied ready TX session')
    origin.add_argument('--nor', type=Path, help='cold boot from NOR paired with --nand')
    p.add_argument('--nand', type=Path, help='NAND paired with --nor; configured flash may contain existing apps/data')
    p.add_argument('--probe', type=Path, default=ROOT / 'build/compatibility-probe/FrondCompatProbe.prc')
    p.add_argument('--probe-version', type=int, choices=(1, 2), default=2,
                   help='v1 service-only legacy probe; v2 requires host selection of a real Palm control')
    p.add_argument('--timeout', type=runner.positive, default=300, help='wall seconds per firmware run, including slot wait')
    return p


def run(args):
    report = {'version': 1, 'status': 'error', 'rows': [], 'error': None,
              'started_at': datetime.now(timezone.utc).isoformat(),
              'hardware_comparison': 'deferred: physical TX unavailable'}
    root = None
    started = time.monotonic()
    try:
        results.require(bool(args.nor) == bool(args.nand), '--nor and --nand must be supplied together')
        output = args.output.resolve()
        if output.exists():
            raise ValueError('Use a new output directory')
        output.mkdir(parents=True, mode=0o700)
        root = output
        report['tooling'] = []
        for name in TOOLING:
            source = ROOT / name
            digest = runner.sha256(source)
            target = root / 'tooling' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target); target.chmod(0o600)
            results.require(runner.sha256(target) == digest, f'Tooling changed while freezing: {source}')
            report['tooling'].append({'path': str(source), 'sha256': digest, 'copy': str(target)})
        frozen = {}
        inputs = [('synthetic', args.core_test)]
        firmware = bool(args.session or args.nor)
        if firmware:
            inputs += [('core', args.core), ('probe', args.probe)]
            inputs += [('session', args.session)] if args.session else [('nor', args.nor), ('nand', args.nand)]
        report['inputs'] = []
        for label, path in inputs:
            source = path.resolve()
            digest = runner.sha256(source)
            target = root / ('frozen-' + label + source.suffix)
            shutil.copyfile(source, target)
            target.chmod(0o700 if label in ('core', 'synthetic') else 0o600)
            results.require(runner.sha256(target) == digest, f'Input changed while freezing: {source}')
            frozen[label] = target
            report['inputs'].append({'role': label, 'path': str(source), 'sha256': digest, 'copy': str(target)})
        report['probe_version'] = args.probe_version
        report['core_source_binding'] = 'not established by this harness; retain independent build manifest'
        # This executable uses synthetic memory and never boots Palm firmware.
        with (root / 'synthetic.log').open('wb') as log:
            completed = subprocess.run([str(frozen['synthetic'])], stdout=log,
                                       stderr=subprocess.STDOUT, timeout=30)
        if completed.returncode:
            raise ValueError(f'Synthetic interpreter tests exited {completed.returncode}; see synthetic.log')
        report['synthetic_binary'] = {'path': str(args.core_test.resolve()), 'sha256': runner.sha256(args.core_test)}
        report['rows'].append({'case': 'pace-execution/native-interworking', 'status': 'pass',
                               'evidence': str(root / 'synthetic.log'), 'level': 'synthetic'})
        if not firmware:
            report['rows'].extend({'case': case, 'status': 'skipped', 'level': 'firmware',
                                   'reason': 'No session or paired NOR/NAND supplied'} for case in results.cases(args.probe_version))
        else:
            session, previous = frozen.get('session'), ''
            report['origin'] = 'saved-session' if session else 'cold-boot-from-supplied-flash'
            report['factory_setup'] = 'not tested; supplied flash may already be configured'
            replay = root / 'gesture.replay'
            replay.write_text('500 pen-down 160 200\n1000 pen-up\n' if args.probe_version == 2 else '')
            replay.chmod(0o600)
            for label in ('initial', 'restored'):
                token = uuid.uuid4().hex
                config = root / (label + '-config.pdb')
                config.write_bytes(results.config_database(token, previous)); config.chmod(0o600)
                boot = ['--session', str(session)] if session else [
                    '--nor', str(frozen['nor']), '--nand', str(frozen['nand'])]
                argv = ['--core', str(frozen['core']), *boot,
                        '--output', str(root / label), '--app', results.APP,
                        '--install', str(config), '--export-db', results.LOG,
                        '--replay', str(replay), '--screenshot', '1500', '--run-for', '3', '--headless',
                        '--timeout', str(args.timeout), '--startup-timeout', str(args.timeout),
                        '--ready-timeout', '60']
                if not previous:
                    argv += ['--install', str(frozen['probe'])]
                run_result = runner.run(runner.parser().parse_args(argv))
                report.setdefault('runs', []).append({'phase': label, 'token': token,
                                                      'result': str(root / label / 'result.json')})
                if run_result['status'] != 'ok':
                    raise ValueError(f'{label}: {run_result["stage"]}: {run_result["error"]}')
                if not session:
                    origin = run_result.get('origin', {})
                    results.require(origin.get('mode') == 'cold-boot' and origin.get('saved_ram_cpu_loaded') is False,
                                    'Runner did not confirm boot without saved RAM/CPU')
                artifacts = run_result.get('artifacts', {})
                results.require(artifacts.get('session_saved') is True, 'Runner did not save completed session')
                exports = artifacts.get('exports', [])
                results.require(len(exports) == 1 and exports[0].get('database') == results.LOG,
                                'Missing named compatibility export')
                archive = Path(exports[0]['path'])
                checks = results.check_records(results.read_log(archive, version=args.probe_version), token,
                                                resumed=bool(previous), version=args.probe_version)
                report['rows'].extend(dict(check, level='firmware', phase=label, evidence=str(archive))
                                      for check in checks)
                if not session:
                    report['rows'].append({'case': 'cold-boot', 'status': 'pass', 'level': 'firmware',
                                           'evidence': str(archive), 'phase': label,
                                           'observation': 'Fresh guest service results after boot without saved RAM/CPU'})
                session, previous = Path(run_result['artifacts']['session']), token
        if not args.nor:
            report['rows'].append({'case': 'cold-boot', 'status': 'skipped', 'level': 'firmware',
                                   'reason': 'No paired NOR/NAND supplied; saved RAM/CPU restore is not cold boot'})
        report['rows'].append({'case': 'reset-persistence', 'status': 'skipped', 'level': 'firmware',
                               'reason': 'Runner has no caller-requested reset phase'})
        for item in report['inputs'] + report['tooling']:
            results.require(runner.sha256(Path(item['path'])) == item['sha256'],
                            'Original input changed: ' + item['path'])
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
