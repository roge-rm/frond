#!/usr/bin/env python3
"""Capture one native Frond stream and retain repeatable PCM/load measurements."""
import argparse
import array
import hashlib
import json
import os
import re
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import time
import uuid


def pcm_metrics(path, skip_seconds=3):
    raw = Path(path).read_bytes()
    if len(raw) % 4:
        raise ValueError('incomplete stereo PCM frame')
    samples = array.array('h', raw)
    if sys.byteorder != 'little':
        samples.byteswap()
    frames = len(samples) // 2
    skip = min(frames, int(skip_seconds * 44100))
    examined = samples[skip * 2:]
    peak = max(abs(min(examined, default=0)), abs(max(examined, default=0)))
    zeros = longest = run = 0
    it = iter(examined)
    for left, right in zip(it, it):
        if left == 0 and right == 0:
            zeros += 1
            run += 1
            longest = max(longest, run)
        else:
            run = 0
    block_bytes = 441 * 4
    body = raw[skip * 4:]
    complete = len(body) // block_bytes
    silent = sum(not any(body[i * block_bytes:(i + 1) * block_bytes])
                 for i in range(complete))
    return {'rate': 44100, 'channels': 2, 'format': 's16le',
            'seconds': frames / 44100, 'skip_seconds': skip / 44100,
            'examined_frames': len(examined) // 2, 'peak': peak,
            'clipped_samples': sum(v in (-32768, 32767) for v in examined),
            'zero_frames': zeros, 'longest_zero_frames': longest,
            'silent_10ms_blocks': silent, 'complete_10ms_blocks': complete,
            'sha256': hashlib.sha256(raw).hexdigest()}


def pulse_list(kind):
    return json.loads(subprocess.check_output(['pactl', '-f', 'json', 'list', kind]))


def cpu_ticks():
    values = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:9]))
    return sum(values), values[3] + values[4]


def stop(process):
    if process and process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def run(args):
    binary, session = args.binary.resolve(), args.session.resolve()
    # Hash before launching: a later rebuild must not silently change attribution.
    identity = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (binary, session)}
    args.output.mkdir(parents=True, exist_ok=False)
    token = uuid.uuid4().hex
    env = dict(os.environ, PULSE_PROP='frond.check_id=' + token)
    runner = ['heavy', 'emulator'] if shutil.which('heavy') else []
    command = runner + [str(binary), str(session)]
    if args.cpu is not None:
        # Allocate guest memory on the chosen CPU's node before starting SDL workers.
        command = runner + ['taskset', '-c', str(args.cpu), str(binary), str(session)]
    if not args.export_databases:
        command += ['--run-for', str(args.seconds)]
    if args.replay:
        replay = args.replay.resolve()
        identity[str(replay)] = hashlib.sha256(replay.read_bytes()).hexdigest()
        lines = []
        for index, line in enumerate(replay.read_text().splitlines()):
            fields = line.split(maxsplit=2)
            if len(fields) == 3 and fields[1] == 'snapshot':
                line = fields[0] + ' snapshot ' + str((args.output / ('snapshot-%03d.png' % index)).resolve())
            lines.append(line)
        prepared = args.output / 'input.replay'
        prepared.write_text('\n'.join(lines) + '\n')
        command += ['--input-replay', str(prepared.resolve())]
    process = recorder = None
    started = time.monotonic()
    loads = []
    result = {'command': command, 'sha256': identity, 'host': os.uname().nodename,
              'cpu_count': os.cpu_count(), 'cpu_affinity': sorted(os.sched_getaffinity(0)), 'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'loadavg_start': os.getloadavg(), 'measurement': 'emulator; excludes physical output device',
              'continuous_fixture': args.continuous, 'replay': str(args.replay) if args.replay else None}
    result['emulation_cpu'] = args.cpu
    try:
        with (args.output / 'frond.log').open('wb') as log, (args.output / 'capture.raw').open('wb') as raw:
            process = subprocess.Popen(command, env=env,
                                       stdin=subprocess.PIPE if args.export_databases else subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + 30
            stream = None
            while process.poll() is None and time.monotonic() < deadline:
                stream = next((s for s in pulse_list('sink-inputs')
                               if s.get('properties', {}).get('frond.check_id') == token), None)
                if stream:
                    break
                time.sleep(.1)
            if not stream:
                raise RuntimeError('Frond PulseAudio stream not found; use the PulseAudio SDL backend')
            if args.cpu is not None:
                pid_text = stream.get('properties', {}).get('application.process.id', '')
                if not pid_text.isdigit():
                    raise RuntimeError('PulseAudio stream does not identify its process')
                pid = int(pid_text)
                if os.getpgid(pid) != process.pid:
                    raise RuntimeError('PulseAudio stream process is outside this benchmark job')
                workers = []
                # Keep audio/GPU workers free to run elsewhere; bind only the emulator thread.
                for task in Path('/proc/%d/task' % pid).iterdir():
                    tid = int(task.name)
                    if tid != pid:
                        try:
                            os.sched_setaffinity(tid, result['cpu_affinity'])
                            workers.append(tid)
                        except ProcessLookupError:
                            pass
                result['emulator_pid'] = pid
                result['emulator_affinity'] = sorted(os.sched_getaffinity(pid))
                result['unbound_worker_threads'] = workers
            sink = next(s for s in pulse_list('sinks') if s['index'] == stream['sink'])
            monitor = sink.get('monitor_source_name', sink.get('monitor_source'))
            if isinstance(monitor, int):
                monitor = next(s['name'] for s in pulse_list('sources') if s['index'] == monitor)
            if not isinstance(monitor, str):
                raise RuntimeError('PulseAudio sink has no monitor source')
            result['capture_start_elapsed_seconds'] = time.monotonic() - started
            recorder = subprocess.Popen(['parec', '--device=' + monitor,
                                         '--monitor-stream=' + str(stream['index']), '--format=s16le',
                                         '--rate=44100', '--channels=2', '--raw'], stdout=raw,
                                        stderr=subprocess.PIPE, start_new_session=True)
            prior = cpu_ticks()
            finish_at = time.monotonic() + args.seconds
            finishing = False
            log_boundary = None
            while process.poll() is None:
                if args.export_databases and not finishing and time.monotonic() >= finish_at:
                    stop(recorder)
                    log_boundary = (args.output / 'frond.log').stat().st_size
                    archive = (args.output / 'ram-databases.zip').resolve()
                    process.stdin.write(('db-export ' + str(archive) + '\nquit\n').encode())
                    process.stdin.flush()
                    finishing = True
                if not finishing and recorder.poll() is not None:
                    raise RuntimeError('PCM recorder exited: ' + recorder.stderr.read().decode())
                if time.monotonic() - started > args.seconds + 60:
                    raise RuntimeError('Frond exceeded run deadline (including slot wait)')
                time.sleep(1)
                now = cpu_ticks()
                total = now[0] - prior[0]
                loads.append({'elapsed_seconds': time.monotonic() - started,
                              'host_cpu_busy_fraction': 1 - (now[1] - prior[1]) / total if total else None,
                              'loadavg': os.getloadavg()})
                prior = now
            result['exit_code'] = process.returncode
            stop(recorder)
        result['pcm'] = pcm_metrics(args.output / 'capture.raw')
        log_data = (args.output / 'frond.log').read_bytes()
        if args.export_databases:
            result['database_export_written'] = 'export written to ' in log_data.decode(errors='replace')
        text = log_data[:log_boundary].decode(errors='replace')
        result['sdl_underruns'] = text.count('audio underrun')
        result['budget_mips_samples'] = [float(v) for v in re.findall(r'cp-uarm @ ([0-9.]+) MIPS', text)]
        result['replay_events'] = [line for line in text.splitlines() if line.startswith('replay event ')]
        # Silence is only a failure criterion for a known continuous fixture.
        result['continuity_pass'] = (process.returncode == 0 and result['sdl_underruns'] == 0
                                     and result['pcm']['seconds'] >= args.seconds - 3
                                     and result['pcm']['peak'] > 0
                                     and result['pcm']['clipped_samples'] == 0
                                     and result['pcm']['silent_10ms_blocks'] == 0) if args.continuous else None
    finally:
        stop(recorder)
        stop(process)
        result['host_load_samples'] = loads
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'host_load_samples'}, indent=2))
    return int(result['exit_code'] != 0 or result['continuity_pass'] is False
               or (args.export_databases and not result['database_export_written']))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('session', type=Path)
    p.add_argument('output', type=Path, help='new result directory; contains private session hashes and PCM')
    p.add_argument('--binary', type=Path, default=Path('out/frond'))
    p.add_argument('--seconds', type=int, default=180)
    p.add_argument('--cpu', type=int, help='Linux CPU for the emulation thread; existing SDL workers regain the original affinity')
    p.add_argument('--replay', type=Path)
    p.add_argument('--export-databases', action='store_true', help='after capture, export all private RAM databases through the console')
    p.add_argument('--continuous', action='store_true', help='require uninterrupted non-clipping PCM after warm-up')
    args = p.parse_args()
    if not 6 <= args.seconds <= 600:
        p.error('seconds must be between 6 and 600')
    if args.cpu is not None and args.cpu not in os.sched_getaffinity(0):
        p.error('CPU must be online and available in the current affinity mask')
    try:
        return run(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        p.exit(2, str(error) + '\n')


if __name__ == '__main__':
    sys.exit(main())
