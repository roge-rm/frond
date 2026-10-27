#!/usr/bin/env python3
"""Bounded private app observations; never infer compatibility from runner success.

Run: python3 tools/app-compatibility.py --manifest PRIVATE.json --output build/NEW
Manifest v1 has core {path,sha256}, apps [{id,version,app,fixture:{path,sha256},
installs:[{path,sha256}],stages:{cold-boot:{skip:reason}, launch:stage,
interaction:stage,persistence:stage,restore:stage}}]. A stage is {skip:reason} or
{run_for:seconds,replay?:{path,sha256},assertions:[oracle,...]}. Oracles are
{kind:'database-records',database:NAME,type:FOUR_ASCII,creator:FOUR_ASCII,
records_sha256:[HASH,...]} or {kind:'visual-review',expected:TEXT,at_ms:[MS,...]}.
Paths are relative to the manifest. All hashes are required. Unknown keys fail.
Launch installs and launches; subsequent stages resume the previous saved state.
Database interaction evidence must differ from launch; persistence/restore must
retain the preceding exact records. Visual assertions always need human review.
A fixture may instead be {mode:'cold-boot',nor:{path,sha256},nand:{path,sha256}}.
Its cold-boot stage must request visual review; a restored session skips cold boot.
launch.resume_app:true resumes a foreground fixture without installs or switching;
this leaves fresh install/launch coverage untested. Only persistence may be skipped
while restore continues from the interaction session, with that source recorded.
No downloads, licenses, shell commands, expressions or external result imports.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import sys
import uuid
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
STAGES = ('cold-boot', 'launch', 'interaction', 'persistence', 'restore')
MAX_SECONDS = 600
MAX_DATABASE = 32 * 1024 * 1024
spec = importlib.util.spec_from_file_location('app_frond_run', ROOT / 'tools/frond-run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def keys(value, required, optional=()):
    require(type(value) is dict and set(required) <= value.keys()
            and value.keys() <= set(required) | set(optional), 'Invalid or unknown schema fields')


def text(value, limit=1024):
    require(type(value) is str and 0 < len(value) <= limit and not any(c in value for c in '\0\r\n'),
            'Expected bounded nonempty text')
    return value


def digest(value):
    require(type(value) is str and re.fullmatch('[0-9a-f]{64}', value), 'Expected SHA256')
    return value


def number(value, low, high):
    require(type(value) in (int, float) and low <= value <= high and
            (type(value) is int or math.isfinite(value)),
            'Expected bounded number')
    return value


def identity(value, base):
    keys(value, ('path', 'sha256'))
    path = (base / text(value['path'], 4096)).resolve(strict=True)
    require(path.is_file(), 'Input must be a local regular file')
    expected = digest(value['sha256'])
    require(runner.sha256(path) == expected, f'Input SHA256 mismatch: {path}')
    return {'path': str(path), 'sha256': expected}


def read_database(path, oracle):
    """Read exact records, excluding unstable Palm header timestamps/IDs."""
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        require(len(entries) == 1, 'Expected exactly one named database')
        item = entries[0]
        require(not item.is_dir() and item.filename == oracle['database'] + '.pdb',
                'Unexpected named database archive member')
        require(item.file_size <= MAX_DATABASE, 'Oversized database')
        require(not item.flag_bits & 1 and item.compress_type in
                (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED), 'Unsupported database archive encoding')
        try:
            raw = archive.read(item)
        except (RuntimeError, NotImplementedError, EOFError, zipfile.BadZipFile, zlib.error) as error:
            raise ValueError(f'Unreadable database archive: {error}') from error
    require(len(raw) >= 78, 'Truncated Palm database')
    require(raw[:32].split(b'\0', 1)[0] == oracle['database'].encode(), 'Wrong internal database name')
    require(raw[60:68] == (oracle['type'] + oracle['creator']).encode('ascii'), 'Wrong database type/creator')
    require(not struct.unpack_from('>H', raw, 32)[0] & 1, 'Resource databases unsupported')
    require(struct.unpack_from('>I', raw, 72)[0] == 0, 'Chained record lists unsupported')
    count = struct.unpack_from('>H', raw, 76)[0]
    end = 78 + count * 8
    require(count > 0 and end <= len(raw), 'Invalid record directory')
    identifiers = set()
    for index in range(count):
        start = 78 + index * 8
        require(not raw[start + 4] & 0xa0, 'Deleted or busy database record')
        identifier = int.from_bytes(raw[start + 5:start + 8], 'big')
        require(identifier != 0 and identifier not in identifiers, 'Invalid or duplicate record identity')
        identifiers.add(identifier)
    offsets = [struct.unpack_from('>I', raw, 78 + i * 8)[0] for i in range(count)] + [len(raw)]
    require(all(end <= a < b <= len(raw) for a, b in zip(offsets, offsets[1:])), 'Invalid record offsets')
    return [hashlib.sha256(raw[a:b]).hexdigest() for a, b in zip(offsets, offsets[1:])]


def validate_replay(path):
    """Restrict matrix inputs to bounded physical controls and remapped snapshots."""
    require(path.stat().st_size <= 1024 * 1024, 'Oversized replay')
    previous, controls = 0, 0
    lines = path.read_text().splitlines()
    require(len(lines) <= 10000, 'Too many replay events')
    for line in lines:
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        fields = line.split(maxsplit=2)
        require(len(fields) >= 2 and re.fullmatch('[0-9]+', fields[0]), 'Invalid replay time')
        at = int(fields[0])
        require(previous <= at <= MAX_SECONDS * 1000, 'Replay outside bounded interval')
        previous = at
        command = fields[1]
        argument = fields[2] if len(fields) == 3 else ''
        if command == 'snapshot':
            require(bool(argument), 'Missing snapshot path')  # runner parses and remaps destination
        elif command == 'pen-up':
            require(not argument, 'Unexpected pen-up argument')
            controls += 1
        elif command in ('pen-down', 'pen-move'):
            require(re.fullmatch(r'[0-9]{1,3} [0-9]{1,3}', argument) is not None,
                    'Invalid pen coordinates')
            require(all(int(x) <= 479 for x in argument.split()), 'Pen coordinates out of bounds')
            controls += 1
        elif command in ('key-down', 'key-up'):
            require(argument in ('up', 'down', 'left', 'right', 'center', 'power', 'calendar',
                                 'contacts', 'mail', 'home'), 'Unsupported replay key')
            controls += 1
        else:
            raise ValueError('Unsupported replay command')
    return controls


def strict_json(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON key')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=unique)


def validate_manifest(raw, base):
    keys(raw, ('version', 'core', 'apps'))
    require(type(raw['version']) is int and raw['version'] == 1, 'Unsupported manifest version')
    plan = {'version': 1, 'core': identity(raw['core'], base), 'apps': []}
    require(type(raw['apps']) is list and 1 <= len(raw['apps']) <= 16, 'Expected 1–16 apps')
    ids = set()
    for app in raw['apps']:
        keys(app, ('id', 'version', 'app', 'fixture', 'installs', 'stages'))
        name = text(app['id'], 64)
        require(re.fullmatch('[a-zA-Z0-9_-]+', name) and name not in ids, 'Invalid/duplicate app id')
        ids.add(name)
        item = {'id': name, 'version': text(app['version']), 'app': text(app['app'], 31),
                'installs': [], 'stages': {}}
        fixture = app['fixture']
        if type(fixture) is dict and fixture.get('mode') == 'cold-boot':
            keys(fixture, ('mode', 'nor', 'nand'))
            item['fixture'] = {'mode': 'cold-boot', 'nor': identity(fixture['nor'], base),
                               'nand': identity(fixture['nand'], base)}
        else:
            item['fixture'] = identity(fixture, base)
        raw_boot = item['fixture'].get('mode') == 'cold-boot'
        require(len(item['app'].encode()) <= 31, 'App name exceeds Palm limit')
        require(type(app['installs']) is list and len(app['installs']) <= 32, 'Too many installs')
        for source in app['installs']:
            record = identity(source, base)
            require(Path(record['path']).suffix.lower() in ('.prc', '.pdb'), 'Install must be PRC/PDB')
            item['installs'].append(record)
        keys(app['stages'], STAGES)
        for label in STAGES:
            stage = app['stages'][label]
            if type(stage) is dict and 'skip' in stage:
                require(not (raw_boot and label == 'cold-boot'), 'Raw fixture requires cold-boot stage')
                keys(stage, ('skip',))
                item['stages'][label] = {'skip': text(stage['skip'])}
                continue
            require(label != 'cold-boot' or raw_boot, 'Session restore cannot establish cold boot')
            keys(stage, ('run_for', 'assertions'), ('replay', 'resume_app') if label == 'launch' else ('replay',))
            checked = {'run_for': number(stage['run_for'], 0, MAX_SECONDS), 'assertions': []}
            if 'resume_app' in stage:
                require(type(stage['resume_app']) is bool, 'resume_app must be boolean')
                checked['resume_app'] = stage['resume_app']
                require(not stage['resume_app'] or not item['installs'], 'Fixture resume cannot install apps')
            if 'replay' in stage:
                checked['replay'] = identity(stage['replay'], base)
                controls = validate_replay(Path(checked['replay']['path']))
            else:
                controls = 0
            require(label != 'interaction' or controls > 0, 'Interaction requires a physical input replay')
            require(type(stage['assertions']) is list and 1 <= len(stage['assertions']) <= 16,
                    'Expected 1–16 explicit assertions')
            databases = set()
            for oracle in stage['assertions']:
                require(type(oracle) is dict, 'Invalid oracle')
                if oracle.get('kind') == 'database-records':
                    keys(oracle, ('kind', 'database', 'type', 'creator', 'records_sha256'))
                    db = text(oracle['database'], 31)
                    require(len(db.encode()) <= 31 and re.fullmatch(r'[A-Za-z0-9 _.-]+', db)
                            and db not in databases, 'Unsupported/duplicate database name')
                    databases.add(db)
                    for field in ('type', 'creator'):
                        require(type(oracle[field]) is str and re.fullmatch('[ -~]{4}', oracle[field]),
                                'Type/creator must be four ASCII bytes')
                    require(type(oracle['records_sha256']) is list and
                            1 <= len(oracle['records_sha256']) <= 4096, 'Expected bounded record hashes')
                    for value in oracle['records_sha256']:
                        digest(value)
                elif oracle.get('kind') == 'visual-review':
                    keys(oracle, ('kind', 'expected', 'at_ms'))
                    text(oracle['expected'])
                    require(type(oracle['at_ms']) is list and 1 <= len(oracle['at_ms']) <= 16,
                            'Expected bounded screenshot times')
                    for at in oracle['at_ms']:
                        require(type(at) is int and 0 <= at <= MAX_SECONDS * 1000, 'Invalid screenshot time')
                else:
                    raise ValueError('Unsupported oracle')
                checked['assertions'].append(oracle.copy())
            require(label != 'cold-boot' or any(o['kind'] == 'visual-review' for o in checked['assertions']),
                    'Cold boot needs explicit visual review; static records cannot prove fresh guest execution')
            item['stages'][label] = checked
        plan['apps'].append(item)
    return plan


def all_inputs(plan):
    yield plan['core']
    for app in plan['apps']:
        if app['fixture'].get('mode') == 'cold-boot':
            yield app['fixture']['nor']
            yield app['fixture']['nand']
        else:
            yield app['fixture']
        yield from app['installs']
        for stage in app['stages'].values():
            if 'replay' in stage:
                yield stage['replay']


def snapshot_inputs(plan, root):
    directory = root / 'inputs'
    directory.mkdir(mode=0o700)
    copies = {}
    for index, record in enumerate(all_inputs(plan)):
        source = Path(record['path'])
        if str(source) not in copies:
            target = directory / (f'{index:04d}' + source.suffix)
            shutil.copyfile(source, target)
            target.chmod(0o700 if record is plan['core'] else 0o600)
            require(runner.sha256(target) == record['sha256'] == runner.sha256(source),
                    f'Input changed during snapshot: {source}')
            copies[str(source)] = target
        record['snapshot'] = str(copies[str(source)])
    return copies


def artifact(path, directory):
    path = Path(path)
    require(path.is_absolute() and path.resolve().is_relative_to(directory.resolve())
            and path.is_file() and not path.is_symlink(), 'Detached or missing artifact')
    return {'path': str(path), 'sha256': runner.sha256(path), 'size': path.stat().st_size}


def verify_runner(result, directory, expected, cold_boot=False):
    require(result.get('status') == 'ok' and result.get('stage') == 'complete'
            and result.get('core_exit_code') == 0,
            f'Runner {result.get("stage")}: {result.get("error") or "did not finish"}')
    require(json.loads((directory / 'result.json').read_text()) == result, 'Detached runner result')
    origin = result.get('origin', {})
    require(origin.get('mode') == ('cold-boot' if cold_boot else 'session-restore')
            and origin.get('saved_ram_cpu_loaded') is (not cold_boot), 'Wrong runner origin')
    actual = result.get('inputs', [])
    require(len(actual) == len(expected), 'Wrong runner input count')
    for observed, wanted in zip(actual, expected):
        require(observed.get('path') == wanted['path'] and observed.get('sha256') == wanted['sha256'],
                'Runner input identity mismatch')
    require(result['artifacts'].get('session_saved') is True, 'Session not saved')
    require(result['artifacts'].get('session') == str(directory / 'working.frond'), 'Detached saved session')
    return artifact(directory / 'working.frond', directory)


def execute_stage(plan, app, label, stage, source, directory, timeout, previous):
    resume = label == 'launch' and stage.get('resume_app', False)
    installs = app['installs'] if label == 'launch' and not resume else []
    argv = ['--core', plan['core']['snapshot'], '--output', str(directory),
            '--run-for', str(stage['run_for']), '--timeout', str(timeout),
            '--startup-timeout', str(timeout), '--headless']
    argv += ['--app', app['app']] if label == 'launch' and not resume else ['--resume-app']
    expected = [{'path': plan['core']['snapshot'], 'sha256': plan['core']['sha256']}]
    if label == 'cold-boot':
        for field in ('nor', 'nand'):
            item = app['fixture'][field]
            argv += ['--' + field, item['snapshot']]
            expected.append({'path': item['snapshot'], 'sha256': item['sha256']})
    else:
        argv += ['--session', str(source)]
        expected.append({'path': str(source), 'sha256': runner.sha256(source)})
    for item in installs:
        argv += ['--install', item['snapshot']]
        expected.append({'path': item['snapshot'], 'sha256': item['sha256']})
    if 'replay' in stage:
        item = stage['replay']
        argv += ['--replay', item['snapshot']]
        expected.append({'path': item['snapshot'], 'sha256': item['sha256']})
    for oracle in stage['assertions']:
        if oracle['kind'] == 'database-records':
            argv += ['--export-db', oracle['database']]
        else:
            for at in oracle['at_ms']:
                argv += ['--screenshot', str(at)]
    # Always run live. There is deliberately no report-import entry point.
    require(not directory.exists(), 'Stage output already exists')
    result = runner.run(runner.parser().parse_args(argv))
    saved = verify_runner(result, directory, expected, cold_boot=label == 'cold-boot')
    row = {'status': 'needs-review' if resume else 'pass', 'saved_session': saved,
           'operation': 'cold-boot' if label == 'cold-boot' else 'fixture-resume' if resume else
                        'install-launch' if label == 'launch' else 'session-resume',
           'observations': [], 'records': {}}
    if resume:
        row['install_launch_coverage'] = 'untested: foreground fixture resumed without app launch'
    exports = result['artifacts']['exports']
    db_oracles = [o for o in stage['assertions'] if o['kind'] == 'database-records']
    require(len(exports) == len(db_oracles), 'Unexpected export count')
    for index, oracle in enumerate(db_oracles, 1):
        exported = exports[index - 1]
        archive = directory / f'database-{index:03d}.zip'
        require(exported == {'database': oracle['database'], 'path': str(archive)}, 'Detached export')
        proof = artifact(archive, directory)
        hashes = read_database(archive, oracle)
        require(hashes == oracle['records_sha256'], f'Unexpected records: {oracle["database"]}')
        prior = previous.get(oracle['database'])
        if label == 'interaction':
            require(prior is not None and hashes != prior, 'No observed interaction database change')
        elif label in ('persistence', 'restore'):
            require(prior == hashes, 'Persistence/restore needs exact preceding database evidence')
        row['records'][oracle['database']] = hashes
        row['observations'].append({'kind': 'database-records', 'database': oracle['database'],
                                    'status': 'pass', 'evidence': proof})
    for oracle in stage['assertions']:
        if oracle['kind'] == 'visual-review':
            captures = result['artifacts']['screenshots']
            evidence = []
            for at in oracle['at_ms']:
                selected = [c for c in captures if c['at_ms'] == at]
                require(selected, 'Missing requested review frame')
                evidence.extend(artifact(c['path'], directory) for c in selected)
            row['observations'].append({'kind': 'visual-review', 'status': 'needs-review',
                                        'expected': oracle['expected'], 'evidence': evidence,
                                        'limit': 'Frame existence and replay completion do not establish app response.'})
            row['status'] = 'needs-review'
    row['artifacts'] = [artifact(p, directory) for p in sorted(directory.iterdir()) if p.is_file()]
    return row


def run(manifest, output, timeout=180):
    report = {'version': 1, 'run_id': uuid.uuid4().hex, 'execution_status': 'error', 'coverage_status': 'incomplete', 'apps': [],
              'started_at': datetime.now(timezone.utc).isoformat(),
              'limits': ['Session restores are not cold boots.',
                         'Exact records establish only the declared observations.',
                         'Host run IDs bind fresh artifacts, not guest-generated freshness tokens.',
                         'Visual observations require human review; no full compatibility verdict.']}
    root, plan = None, None
    try:
        number(timeout, .001, MAX_SECONDS)
        output = output.resolve()
        build = (ROOT / 'build').resolve()
        require(not output.is_relative_to(ROOT) or
                (output.is_relative_to(build) and output != build),
                'Output inside the checkout must be under build/; otherwise use a private external directory')
        require(not output.exists(), 'Use a new output directory')
        manifest = manifest.resolve(strict=True)
        require(manifest.stat().st_size <= 2 * 1024 * 1024, 'Oversized manifest')
        raw = manifest.read_bytes()
        plan = validate_manifest(strict_json(raw), manifest.parent)
        output.mkdir(parents=True, mode=0o700)
        root = output
        (root / 'manifest.json').write_bytes(raw)
        (root / 'manifest.json').chmod(0o600)
        report['manifest'] = {'path': str(manifest), 'sha256': hashlib.sha256(raw).hexdigest()}
        snapshot_inputs(plan, root)
        report['plan'] = plan
        for app in plan['apps']:
            app_report = {'id': app['id'], 'version': app['version'],
                          'version_source': 'manifest declaration; not independently verified',
                          'persistence_scope': 'whole-session saved state; no app relaunch or flash persistence',
                          'stages': {}}
            report['apps'].append(app_report)
            raw_boot = app['fixture'].get('mode') == 'cold-boot'
            source = None if raw_boot else Path(app['fixture']['snapshot'])
            previous, blocked = {}, False
            source_stage, source_sha = 'fixture', None if raw_boot else app['fixture']['sha256']
            for label in STAGES:
                stage = app['stages'][label]
                row = {'run_id': uuid.uuid4().hex, 'status': 'blocked'}
                app_report['stages'][label] = row
                if 'skip' in stage:
                    row.update(status='skipped', reason=stage['skip'])
                    if label not in ('cold-boot', 'persistence'):
                        blocked = True
                    continue
                if blocked:
                    row['reason'] = 'A preceding stage failed or was skipped'
                    continue
                directory = root / app['id'] / label
                try:
                    for record in all_inputs(plan):
                        require(runner.sha256(Path(record['snapshot'])) == record['sha256'],
                                'Frozen input snapshot changed')
                    if label == 'cold-boot':
                        row['input_mode'] = 'cold-boot'
                    else:
                        require(runner.sha256(source) == source_sha, 'Saved session chain changed')
                        row['input_session'] = {'path': str(source), 'sha256': source_sha,
                                                'source_stage': source_stage}
                    row.update(execute_stage(plan, app, label, stage, source, directory, timeout, previous))
                    source, previous = Path(row['saved_session']['path']), row['records']
                    source_stage, source_sha = label, row['saved_session']['sha256']
                except (ValueError, OSError, KeyError, zipfile.BadZipFile, UnicodeError) as error:
                    row.update(status='error', error=str(error))
                    if directory.exists():
                        row['artifacts'] = [artifact(p, directory) for p in sorted(directory.iterdir())
                                            if p.is_file() and not p.is_symlink()]
                    blocked = True
        report['execution_status'] = 'ok'
    except (ValueError, OSError, KeyError, zipfile.BadZipFile, UnicodeError) as error:
        report.update(execution_status='error', error=str(error))
    except KeyboardInterrupt:
        report.update(execution_status='error', error='Interrupted; unfinished stages are not passes')
    finally:
        if root:
            checks = []
            for record in all_inputs(plan):
                try:
                    unchanged = runner.sha256(Path(record['path'])) == record['sha256']
                    frozen = runner.sha256(Path(record['snapshot'])) == record['sha256']
                except (OSError, KeyError):
                    unchanged = frozen = False
                checks.append({'path': record['path'], 'sha256': record['sha256'],
                               'unchanged': unchanged, 'snapshot_unchanged': frozen})
            try:
                manifest_unchanged = (runner.sha256(manifest) == report['manifest']['sha256']
                                      == runner.sha256(root / 'manifest.json'))
            except (OSError, KeyError):
                manifest_unchanged = False
            report['manifest_unchanged'] = manifest_unchanged
            report['input_preservation'] = checks
            if not manifest_unchanged or not all(c['unchanged'] and c['snapshot_unchanged'] for c in checks):
                report.update(execution_status='error', error='Input preservation failed')
            if any(row['status'] == 'error' for app in report['apps'] for row in app['stages'].values()):
                report['execution_status'] = 'error'
            statuses = [row['status'] for app in report['apps'] for row in app['stages'].values()]
            complete = (report['execution_status'] == 'ok' and
                        len(statuses) == len(plan['apps']) * len(STAGES) and
                        all(s == 'pass' for s in statuses))
            report['coverage_status'] = ('complete' if complete else
                                         'needs-review' if 'needs-review' in statuses else 'incomplete')
            # Private outputs may contain personal data even when the source session is synthetic.
            for path in root.rglob('*'):
                if path.is_dir() and not path.is_symlink():
                    path.chmod(0o700)
                if path.is_file() and not path.is_symlink():
                    path.chmod(0o700 if path.stat().st_mode & 0o111 else 0o600)
            (root / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n')
            (root / 'matrix.json').chmod(0o600)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=runner.positive, default=180)
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    result = run(args.manifest, args.output, args.timeout)
    print(json.dumps(result))
    # Successful execution is separate from incomplete/review-required coverage.
    return 0 if result['execution_status'] == 'ok' else 1


if __name__ == '__main__':
    sys.exit(main())
