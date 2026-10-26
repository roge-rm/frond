#!/usr/bin/env python3
"""Run the reproducible TX acceptance checks using user-supplied images."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import shutil
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run_command(command, logfile, timeout, env):
    """Interrupt our process group on timeout so Python acceptance tests clean up their child emulator."""
    with logfile.open('xb') as output:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return process.wait(timeout=timeout), False
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGINT)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            return process.returncode, True
        except BaseException:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGINT)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            raise


def heavy_command(kind, command):
    return (['heavy', kind] if shutil.which('heavy') else []) + command


def check_png(path):
    data = path.read_bytes()
    if len(data) > 16 * 1024 * 1024 or data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('core did not produce a valid PNG framebuffer')
    offset, header, images, ended = 8, None, [], False
    while offset + 12 <= len(data):
        length = struct.unpack_from('>I', data, offset)[0]
        end = offset + 12 + length
        if end > len(data):
            raise ValueError('truncated PNG framebuffer')
        kind, payload = data[offset + 4:offset + 8], data[offset + 8:end - 4]
        checksum = struct.unpack_from('>I', data, end - 4)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != checksum:
            raise ValueError('PNG framebuffer checksum mismatch')
        if offset == 8 and kind != b'IHDR':
            raise ValueError('PNG framebuffer has no header')
        if kind == b'IHDR':
            if header is not None or length != 13:
                raise ValueError('invalid PNG framebuffer header')
            header = struct.unpack('>IIBBBBB', payload)
        elif kind == b'IDAT':
            images.append(payload)
        elif kind == b'IEND':
            if length or end != len(data):
                raise ValueError('invalid PNG framebuffer end')
            ended = True
            break
        offset = end
    if header != (320, 480, 8, 6, 0, 0, 0) or not images or not ended:
        raise ValueError('unexpected or incomplete TX framebuffer')
    row_size = 1 + 320 * 4
    expected = row_size * 480
    decoder = zlib.decompressobj()
    try:
        pixels = decoder.decompress(b''.join(images), expected + 1)
    except zlib.error as error:
        raise ValueError('invalid compressed PNG framebuffer') from error
    if (len(pixels) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or
            any(pixels[row * row_size] > 4 for row in range(480))):
        raise ValueError('invalid PNG framebuffer image data')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', type=Path, required=True, help='your baseline TX session (read-only)')
    parser.add_argument('--output', type=Path, required=True, help='new private result directory')
    parser.add_argument('--core', type=Path, default=ROOT / 'out/frond')
    parser.add_argument('--nor', type=Path, help='your TX NOR, paired with --nand, for a cold-boot capture')
    parser.add_argument('--nand', type=Path, help='your TX NAND, paired with --nor')
    parser.add_argument('--sd-probe', type=Path, help='locally built FrondSDProbe.prc')
    parser.add_argument('--network-probe', type=Path, help='locally built FrondNetProbe.prc')
    parser.add_argument('--wifi-session', type=Path, help='TX session with the Frond Wi-Fi profile saved')
    parser.add_argument('--wifi-probe', type=Path, help='locally built FrondHostProbe.prc; requires --wifi-session')
    parser.add_argument('--audio-session', type=Path, help='playing TX session for optional PCM capture')
    parser.add_argument('--audio-seconds', type=int, default=180, help='audio capture duration, 6–600 seconds')
    parser.add_argument('--audio-continuous', action='store_true', help='require continuity for a known continuous fixture')
    parser.add_argument('--skip-synthetic', action='store_true', help='skip make test and make test-desktop')
    parser.add_argument('--cold-seconds', type=int, default=60, help='cold boot capture time (default: 60)')
    args = parser.parse_args()
    if bool(args.nor) != bool(args.nand):
        parser.error('--nor and --nand must be supplied together')
    if bool(args.wifi_session) != bool(args.wifi_probe):
        parser.error('--wifi-session and --wifi-probe must be supplied together')
    if args.audio_continuous and not args.audio_session:
        parser.error('--audio-continuous requires --audio-session')
    if not 6 <= args.audio_seconds <= 600:
        parser.error('--audio-seconds must be between 6 and 600')
    if not 1 <= args.cold_seconds <= 600:
        parser.error('--cold-seconds must be between 1 and 600')
    inputs = {}
    for name in ('session', 'nor', 'nand', 'sd_probe', 'network_probe', 'wifi_session', 'wifi_probe', 'audio_session'):
        path = getattr(args, name)
        if path is not None:
            try:
                path = path.resolve(strict=True)
            except FileNotFoundError:
                parser.error(f'{name} does not exist: {path}')
            if not path.is_file():
                parser.error(f'{name} must be a file: {path}')
            inputs[name] = path
    core = args.core.resolve()
    if args.skip_synthetic and not core.is_file():
        parser.error('--core must exist when synthetic checks are skipped')
    output = args.output.resolve()
    try:
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    except FileExistsError:
        parser.error(f'output already exists: {output}')
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', QT_QPA_PLATFORM='offscreen')
    sources = {name: {'path': str(path), 'sha256': digest(path)} for name, path in inputs.items()}
    report = {'measurement': 'emulator, not a physical Palm TX or physical audio output',
              'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'host': platform.node(), 'kernel': platform.release(), 'architecture': platform.machine(),
              'working_directory': str(ROOT),
              'cpu_count': os.cpu_count(), 'cpu_affinity': sorted(os.sched_getaffinity(0)),
              'runner_sha256': digest(Path(__file__).resolve()), 'loadavg_start': os.getloadavg(),
              'inputs': sources, 'steps': [], 'manual_checks_required': [], 'outcome': 'running'}
    scripts = ['tests/desktop-control.py']
    for input_name, script in [('sd_probe', 'tests/desktop-sd.py'),
                               ('network_probe', 'tests/desktop-network.py'),
                               ('wifi_probe', 'tests/desktop-host-network.py'),
                               ('audio_session', 'tools/check-native-audio.py')]:
        if input_name in inputs:
            scripts.append(script)
    scripts.append('tests/fixtures/frond-pattern.pdb')
    report['test_files'] = {name: digest(ROOT / name) for name in scripts}
    try:
        report['cpu_model'] = next((line.split(':', 1)[1].strip()
                                    for line in Path('/proc/cpuinfo').read_text().splitlines()
                                    if line.startswith('model name')), None)
    except OSError:
        report['cpu_model'] = None
    try:
        report['git_commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        report['git_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT))
    except (OSError, subprocess.SubprocessError):
        report['git_commit'] = None
        report['git_dirty'] = None

    def save():
        temporary = output / 'result.json.tmp'
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(output / 'result.json')

    def step(name, command, timeout, validator=None, step_env=None):
        row = {'name': name, 'command': [str(part) for part in command],
               'log': name + '.log', 'status': 'running'}
        report['steps'].append(row); save()
        started = time.monotonic()
        try:
            code, timed_out = run_command(row['command'], output / row['log'], timeout, step_env if step_env is not None else env)
            row['exit_code'] = code
            if timed_out:
                row['status'] = 'timeout'
            elif code:
                row['status'] = 'failed'
            else:
                if validator:
                    validator()
                row['status'] = 'passed'
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            row.update(status='failed', error=str(error))
        row['elapsed_seconds'] = round(time.monotonic() - started, 3)
        print(name + ': ' + row['status'], flush=True)
        save()
        return row['status'] == 'passed'

    def skipped(name, reason):
        report['steps'].append({'name': name, 'status': 'skipped', 'reason': reason})
        print(name + ': skipped (' + reason + ')', flush=True)
        save()

    try:
        save()
        if args.skip_synthetic:
            skipped('synthetic', 'explicitly skipped')
        else:
            if not step('synthetic', heavy_command('build', ['make', '-j4', 'test', 'test-desktop']), 1800):
                raise RuntimeError('synthetic checks failed; remaining checks were not run')
        report['core'] = {'path': str(core), 'sha256': digest(core),
                          'version': subprocess.check_output([str(core), '--version'], text=True, timeout=10).strip()}
        save()
        if args.nor:
            png = output / 'cold-boot.png'
            command = heavy_command('emulator', [str(core), '--experimental-tx', str(inputs['nor']),
                       '--nand', str(inputs['nand']), '--no-sound', '--run-for', str(args.cold_seconds),
                       '--framebuffer-output', str(png)])
            if step('cold-boot', command, args.cold_seconds + 120, lambda: check_png(png)):
                report['steps'][-1]['note'] = 'Framebuffer captured; inspect it to confirm a usable Palm screen.'
                report['manual_checks_required'].append('Inspect cold-boot.png for a usable Palm screen.')
                save()
        else:
            skipped('cold-boot', 'NOR and NAND were not supplied')
        step('desktop-control', [sys.executable, 'tests/desktop-control.py', '--core', str(core),
                                 '--session', str(inputs['session'])], 600)
        for name, option, script in [('sd', 'sd_probe', 'tests/desktop-sd.py'),
                                     ('network', 'network_probe', 'tests/desktop-network.py')]:
            if option not in inputs:
                skipped(name, 'locally built probe was not supplied'); continue
            step(name, [sys.executable, script, '--core', str(core), '--session', str(inputs['session']),
                        '--probe', str(inputs[option]), '--output', str(output / (name + '-results'))], 900)
        if 'wifi_session' not in inputs:
            skipped('wifi', 'Wi-Fi session and locally built probe were not supplied')
        else:
            step('wifi', [sys.executable, 'tests/desktop-host-network.py', '--wireless', '--core', str(core),
                          '--session', str(inputs['wifi_session']), '--probe', str(inputs['wifi_probe']),
                          '--output', str(output / 'wifi-results')], 900)
        if 'audio_session' not in inputs:
            skipped('audio', 'playing session was not supplied')
        else:
            command = [sys.executable, 'tools/check-native-audio.py', str(inputs['audio_session']),
                       str(output / 'audio-results'), '--binary', str(core), '--seconds', str(args.audio_seconds)]
            if args.audio_continuous:
                command.append('--continuous')
            step('audio', command, args.audio_seconds + 150,
                 step_env=dict(env, SDL_AUDIODRIVER='pulseaudio'))
            if not args.audio_continuous:
                report['steps'][-1]['note'] = 'Continuity was not asserted; inspect captured PCM and result.json.'
                save()
        changed = [name for name, path in inputs.items() if digest(path) != sources[name]['sha256']]
        if digest(core) != report['core']['sha256']:
            changed.append('core')
        if digest(Path(__file__).resolve()) != report['runner_sha256']:
            changed.append('runner')
        changed.extend(name for name, expected in report['test_files'].items()
                       if digest(ROOT / name) != expected)
        report['unchanged_inputs'] = not changed
        if changed:
            report['changed_inputs'] = changed
        report['coverage'] = ('partial' if any(s['status'] == 'skipped' for s in report['steps'])
                              else 'all configured automated checks')
        report['outcome'] = 'failed' if changed or any(s['status'] in ('failed', 'timeout') for s in report['steps']) else 'passed'
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(outcome='failed', error=str(error))
    except KeyboardInterrupt:
        report.update(outcome='interrupted')
        raise
    finally:
        report['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
        report['loadavg_end'] = os.getloadavg()
        save()
    print('Baseline:', report['outcome'], '; results:', output, flush=True)
    return 0 if report['outcome'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
