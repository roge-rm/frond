"""Boot-once session fixtures for the real-firmware tests.

A fixture is a session saved at a known screen, kept in build/fixtures/<name>-<hash>/ as launcher.session (a
.frond file, or for ARM devices a machine folder holding one) with the build's logs in logs/. The hash covers
the core binary, the firmware files, a tag for how the session was made and the scripts that make it, so a new
core or ROM builds a new one. A fixture whose file doesn't match its manifest is rebuilt. Fixtures hold private
firmware state: build/ is ignored and the folder is mode 0700.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import palm_boot

FIXTURES = ROOT / 'build/fixtures'
FORMAT = 1


class Fixture:
    def __init__(self, session, logs, built):
        self.session, self.logs, self.built = session, logs, built

    def copy_to(self, dest):
        """Copies the session to dest for a test to load or change; the fixture itself is never handed out."""
        dest = Path(dest)
        if self.session.is_dir():
            shutil.copytree(self.session, dest)
        else:
            shutil.copyfile(self.session, dest)
            dest.chmod(0o600)
        return dest

    def copy_logs(self, dest):
        """Copies the build's logs and frames into dest, for evidence."""
        for path in sorted(self.logs.iterdir()):
            if path.is_file():
                shutil.copyfile(path, Path(dest) / path.name)


def file_hash(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tree_hash(path):
    """The SHA-256 of a file, or of every file's name and contents in a folder."""
    path = Path(path)
    if path.is_file():
        return file_hash(path)
    digest = hashlib.sha256()
    for item in sorted(p for p in path.rglob('*') if p.is_file()):
        digest.update(f'{item.relative_to(path)}\0{file_hash(item)}\0'.encode())
    return digest.hexdigest()


def fixture(name, core, inputs, build, tag='', sources=()):
    """Returns the Fixture for name, building it with build(session, logs) when missing, stale or damaged.

    core is the runner binary, inputs the firmware files and tag anything else the session depends on (recipe,
    engine, options). build boots the machine, saves the session at session (a path without .frond, as the
    tests have always used) and writes its logs into the logs folder, which it makes.
    """
    parts = {'format': FORMAT, 'name': name, 'tag': tag, 'core': file_hash(core),
             'inputs': [file_hash(p) for p in inputs],
             'sources': [file_hash(p) for p in (Path(palm_boot.__file__), Path(__file__), *sources)]}
    key = hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()
    stem = f'{name}-{key[:16]}'
    FIXTURES.mkdir(parents=True, exist_ok=True)
    FIXTURES.chmod(0o700)
    folder, manifest = FIXTURES / stem, FIXTURES / f'{stem}.json'
    session, logs = folder / 'launcher.session', folder / 'logs'
    # One builder per fixture; another test process waits here and then reuses it.
    with (FIXTURES / f'.{stem}.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            saved = json.loads(manifest.read_text())
            if (saved['key'] == key and logs.is_dir() and session.exists()
                    and tree_hash(session) == saved['sha256']):
                print(f'boot fixture: reusing {stem}', file=sys.stderr, flush=True)
                return Fixture(session, logs, False)
            print(f'boot fixture: {stem} does not match its manifest, rebuilding', file=sys.stderr, flush=True)
        except (OSError, ValueError, KeyError, TypeError):
            pass
        prune(name, parts['core'], parts['inputs'], stem)
        start = time.monotonic()
        work = Path(tempfile.mkdtemp(prefix=f'.{stem}-', dir=FIXTURES))
        try:
            build(work / 'launcher.session', work / 'logs')
            made = palm_boot.session_file(work / 'launcher.session')
            if not made.is_file() or made.stat().st_size == 0:
                raise AssertionError(f'{name}: fixture session not saved')
            work.rename(folder)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        temp = manifest.with_suffix('.json.tmp')
        temp.write_text(json.dumps({'key': key, 'sha256': tree_hash(session), **parts}, indent=2) + '\n')
        os.replace(temp, manifest)
        print(f'boot fixture: built {stem} in {time.monotonic() - start:.1f} s', file=sys.stderr, flush=True)
        return Fixture(session, logs, True)


def prune(name, core, inputs, keep):
    """Deletes fixtures for name made by another core build from the same firmware, and whatever is left of keep.

    Fixtures for other firmware or tags are kept. One whose lock is held is being built or used and is left.
    """
    for manifest in FIXTURES.glob(f'{name}-*.json'):
        stem = manifest.name[:-len('.json')]
        if '-' in stem[len(name) + 1:] or stem == keep:
            continue  # Another fixture whose name starts with this one.
        try:
            saved = json.loads(manifest.read_text())
            stale = saved['inputs'] == inputs and saved['core'] != core
        except (OSError, ValueError, KeyError, TypeError):
            stale = True
        if not stale:
            continue
        with (FIXTURES / f'.{stem}.lock').open('w') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                continue
            manifest.unlink(missing_ok=True)
            shutil.rmtree(FIXTURES / stem, ignore_errors=True)
    (FIXTURES / f'{keep}.json').unlink(missing_ok=True)
    shutil.rmtree(FIXTURES / keep, ignore_errors=True)


def launcher_fixture(device, nor, nand=None, engine='exact', variant=None):
    """A session at the launcher after the device's setup recipe, from a cold boot of nor (and nand).

    The logs hold setup.log and the setup frames. The build fails if the launcher check doesn't pass.
    """
    image = ['--device', device, nor, '--engine', engine] + (['--nand', nand] if nand else [])
    core_path = palm_boot.core_for(device)

    def build(session, logs):
        logs.mkdir()
        core = palm_boot.Core(image, logs / 'setup.log')
        try:
            if device in palm_boot.DEVICES_68K:
                # The core may wait for a free emulator slot first.
                end = time.monotonic() + 900
                while time.monotonic() < end and not any(line.startswith('frond-68k:') for line in core.lines):
                    time.sleep(1)
            palm_boot.setup_device(device, core, logs, variant=variant)
            core.wait_frame(logs / 'launcher.png', palm_boot.launcher_check(device, variant), 60)
            core.send(f'save-session "{session}"', 'wrote session')
        finally:
            code = core.close()
        if code != 0:
            raise AssertionError(f'{device}: setup core exited {code}, see {logs / "setup.log"}')

    variant_id = variant['id'] if isinstance(variant, dict) else variant
    return fixture(device, core_path, [p for p in (nor, nand) if p], build,
                   tag=f'launcher:{engine}:{variant_id or ""}')
