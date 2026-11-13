#!/usr/bin/env python3
"""Zire 72 Voice Memo records a fake 1 kHz microphone through the real ROM.

source MAIN OUT   writes a copy of native/main.cpp that attaches a fake 1 kHz source of peak
                  FROND_TEST_MIC_LEVEL once the guest is awake; the core build links it as a
                  test-only binary.
run --core CORE --session SESSION --output DIR
                  records with the voice button held at several input levels, saves during a
                  recording and loads that save in a new process, exports each memo and checks
                  its tone. SESSION is a Zire 72 session at the launcher with the setup
                  Reminder up (the smoke session); private, so it's never copied anywhere else.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time
import zipfile

FAKE = r'''
#include <cmath>
#include "audio_input.h"
namespace {
struct TestMicrophone {
    double phase{};
    uint64_t samples{};
    uint32_t rate{};
    bool armPending{};
    AudioInputSource source{};
};
TestMicrophone testMicrophone;
}
'''

ATTACH = '''                if (testMicrophone.armPending && !static_cast<SocPXA*>(soc)->PowerSleeping()) {
                    static_cast<SocPXA*>(soc)->SetMicrophoneSource(&testMicrophone.source);
                    testMicrophone.armPending = false;
                    std::cerr << "test microphone armed" << std::endl;
                }
'''

SETUP = '''        if (const char* level = getenv("FROND_TEST_MIC_LEVEL")) {
            testMicrophone.source = {&testMicrophone,
                [](void* p, AudioInputRoute, uint32_t n, uint32_t d) -> int16_t {
                    auto& f = *static_cast<TestMicrophone*>(p);
                    if (f.rate != n / d) std::cerr << "test microphone rate " << n << "/" << d << std::endl;
                    f.rate = n / d;
                    ++f.samples;
                    const auto value = int16_t(std::lround(atof(getenv("FROND_TEST_MIC_LEVEL")) * std::sin(f.phase)));
                    f.phase = std::fmod(f.phase + 6.283185307179586 * 1000 * d / n, 6.283185307179586);
                    return value;
                },
                [](void*, bool fenced) { std::cerr << "test microphone fenced " << fenced << std::endl; },
                [](void* p) { std::cerr << "test microphone disarmed after " << static_cast<TestMicrophone*>(p)->samples << std::endl; }};
            testMicrophone.armPending = level[0] != 0;
        }
'''


def make_source(main, out):
    s = Path(main).read_text()
    anchor = '                if (hasMicrophone) microphone.Service('
    setup_anchor = '        SdlMicrophone microphone;\n'
    assert s.count(anchor) == 1 and s.count(setup_anchor) == 1
    s = s.replace(anchor, ATTACH + anchor).replace(setup_anchor, SETUP + setup_anchor)
    first_include = s.index('#include')
    s = s[:first_include] + '#include <iostream>\n' + s[first_include:]
    end = s.index('\n', s.rindex('#include')) + 1
    s = s[:end] + FAKE + s[end:]
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text(s)


class Core:
    def __init__(self, core, machine, level, log, extra=(), patience=1):
        # patience scales the waits, for a core that runs slower than real time (--emulated-time).
        self.patience = patience
        self.sock = Path(machine).parent / 'control.sock'
        if self.sock.exists():
            self.sock.unlink()
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(str(self.sock))
        self.server.listen(1)
        self.server.settimeout(60)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', FROND_TEST_MIC_LEVEL=str(level))
        self.log = open(log, 'w')
        self.process = subprocess.Popen([core, str(machine), '--device', 'palm-zire-72', '--desktop-socket',
                                         str(self.sock), '--no-sound', *extra], env=env, stdout=self.log, stderr=self.log)
        self.connection, _ = self.server.accept()
        self.connection.settimeout(60)
        self.reader = self.connection.makefile('rb')
        self.serial = 0

    def request(self, command, argument=''):
        self.serial += 1
        self.connection.sendall(f'{self.serial}\t{command}\t{argument.encode().hex()}\n'.encode())
        while True:
            fields = self.reader.readline().decode().rstrip('\n').split('\t')
            if fields[0] == str(self.serial):
                return fields[1] == 'ok', fields[2], bytes.fromhex(fields[3]).decode(errors='replace') if len(fields) > 3 else ''

    def wait_ready(self):
        deadline = time.monotonic() + 60 * self.patience
        while not self.request('guest-ready')[0]:
            assert time.monotonic() < deadline, 'guest never ready'
            time.sleep(.2)

    def replay(self, path):
        ok, code, message = self.request('input-replay', str(path))
        assert ok, (code, message)
        deadline = time.monotonic() + 120 * self.patience
        while self.request('replay-state')[1] != 'finished':
            assert time.monotonic() < deadline, 'replay never finished'
            time.sleep(.2)

    def memos(self):
        deadline = time.monotonic() + 30
        while True:
            ok, code, listing = self.request('db-list')
            if ok or code != 'not-ready' or time.monotonic() > deadline:
                break
            time.sleep(.5)
        assert ok, (code, listing)
        names = []
        for line in listing.splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[0] == '7374726d' and parts[1] == '56706164':  # strm/Vpad
                names.append(bytes.fromhex(parts[3]).decode('latin-1'))
        return names

    def export(self, name, archive):
        ok, code, message = self.request('export-db', f'{archive}\n{name}')
        assert ok, (code, message)

    def quit(self):
        self.request('quit')
        self.process.wait(30)
        self.log.close()
        self.server.close()
        self.sock.unlink(missing_ok=True)


def wave_from_archive(archive, out):
    """A file stream database: DBLK blocks holding the WAVE file, as on the TG50."""
    with zipfile.ZipFile(archive) as z:
        for path in z.namelist():
            b = z.read(path)
            if len(b) < 78:
                continue
            n = struct.unpack_from('>H', b, 76)[0]
            offsets = [struct.unpack_from('>I', b, 78 + 8 * i)[0] for i in range(n)] + [len(b)]
            clip = bytearray()
            for start, end in zip(offsets, offsets[1:]):
                assert b[start:start + 4] == b'DBLK'
                clip += b[start + 8:start + 8 + struct.unpack_from('>I', b, start + 4)[0]]
            assert clip[:4] == b'RIFF' and clip[8:12] == b'WAVE', clip[:16]
            out.write_bytes(clip)
            return clip
    raise AssertionError('no stream in ' + str(archive))


def analyse(wave):
    clip = wave.read_bytes()
    at, chunks = 12, {}
    while at + 8 <= len(clip):
        size = struct.unpack_from('<I', clip, at + 4)[0]
        chunks[bytes(clip[at:at + 4])] = (at + 8, size)
        at += 8 + size + (size & 1)
    tag, channels, rate = struct.unpack_from('<HHI', clip, chunks[b'fmt '][0])
    raw = wave.with_suffix('.s16')
    subprocess.run(['ffmpeg', '-v', 'error', '-i', str(wave), '-f', 's16le', '-ac', '1', '-acodec', 'pcm_s16le',
                    '-y', str(raw)], check=True)
    data = raw.read_bytes()
    samples = struct.unpack(f'<{len(data) // 2}h', data)
    windows = []
    for start in range(0, len(samples) - rate // 2 + 1, rate // 2):
        x = samples[start:start + rate // 2]
        rms = math.sqrt(sum(v * v for v in x) / len(x))
        # Goertzel near 1 kHz against the window's whole energy. The guest's clock puts the tone
        # a few Hz off, so take the strongest of 990 to 1010 Hz in 1 Hz steps.
        def goertzel(freq):
            c, s1, s2 = 2 * math.cos(2 * math.pi * freq / rate), 0.0, 0.0
            for v in x:
                s1, s2 = v + c * s1 - s2, s1
            return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / len(x) * math.sqrt(2)
        tone = max(goertzel(f) for f in range(990, 1011))
        crossings = sum(1 for a, b in zip(x, x[1:]) if (a < 0) != (b < 0))
        windows.append({'start': start / rate, 'rms': round(rms, 2), 'tone_rms': round(tone, 2),
                        'frequency': round(crossings / 2 / (len(x) / rate), 1), 'peak': max(abs(v) for v in x)})
    return {'tag': tag, 'channels': channels, 'rate': rate, 'samples': len(samples),
            'seconds': len(samples) / rate, 'windows': windows}


# Tap OK on the setup Reminder.
DISMISS = '500 pen-down 50 280\n600 pen-up\n1500 pen-up\n'


def record(args, name, level, events, save_at=None):
    work = args.output / name
    machine = work / 'machine'
    machine.mkdir(parents=True)
    shutil.copy(args.session, machine / 'machine.frond')
    os.chmod(machine / 'machine.frond', 0o600)
    replay = work / 'input.replay'
    replay.write_text(events + f'{int(events.split()[-2]) + 1} snapshot "{work / "end.png"}"\n')
    dismiss = work / 'dismiss.replay'
    dismiss.write_text(DISMISS)
    core = Core(args.core, machine, level, work / 'core.log')
    try:
        core.wait_ready()
        core.replay(dismiss)
        core.wait_ready()
        before = set(core.memos())
        if save_at is None:
            core.replay(replay)
        else:
            ok, code, message = core.request('input-replay', str(replay))
            assert ok, (code, message)
            time.sleep(save_at)
            ok, code, message = core.request('save', str(work / 'during.frond'))
            assert ok, (code, message)
            while core.request('replay-state')[1] != 'finished':
                time.sleep(.2)
        time.sleep(2)
        memos = [m for m in core.memos() if m not in before]
        assert len(memos) == 1, memos
        core.export(memos[0], work / 'memo.zip')
        core.request('save', str(work / 'after.frond'))
    finally:
        core.quit()
    wave_from_archive(work / 'memo.zip', work / 'memo.wav')
    result = analyse(work / 'memo.wav')
    result.update({'memo': memos[0], 'input_peak': level})
    (work / 'metrics.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def load_saved(args, name, saved):
    """Loads a session saved during a recording in a new process; the load lets go of the voice
    button, so Voice Memo finishes that memo."""
    work = args.output / name
    machine = work / 'machine'
    machine.mkdir(parents=True)
    shutil.copy(saved, machine / 'machine.frond')
    core = Core(args.core, machine, 200, work / 'core.log')
    try:
        core.wait_ready()
        home = work / 'home.replay'
        home.write_text(f'2000 pen-down 23 352\n2100 pen-up\n3500 pen-up\n3501 snapshot "{work / "end.png"}"\n')
        core.replay(home)
        time.sleep(1)
        memos = core.memos()
        assert memos, 'no memo after loading'
        newest = sorted(memos)[-1]
        core.export(newest, work / 'memo.zip')
    finally:
        core.quit()
    wave_from_archive(work / 'memo.zip', work / 'memo.wav')
    result = analyse(work / 'memo.wav')
    (work / 'metrics.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def source_rms(level, rate=8000, frequency=1000):
    """RMS of the fake source over one period, rounded as C lround does."""
    n = rate // frequency
    values = [math.copysign(math.floor(abs(level * math.sin(2 * math.pi * i / n)) + .5),
                            math.sin(2 * math.pi * i / n)) for i in range(n)]
    return math.sqrt(sum(v * v for v in values) / n)


def tone(result, first, last):
    """The 1 kHz windows between first and last seconds of a memo."""
    return [w for w in result['windows'] if first <= w['start'] < last]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('source')
    s.add_argument('main')
    s.add_argument('out')
    r = sub.add_parser('run')
    r.add_argument('--core', required=True)
    r.add_argument('--session', type=Path, required=True)
    r.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.mode == 'source':
        make_source(args.main, args.out)
        return
    args.output = args.output.resolve()
    args.core = str(Path(args.core).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    os.chmod(args.output, 0o700)
    # Hold the voice button for seven seconds (Voice Memo starts recording about two seconds
    # in), then tap the silkscreen Home to go back to the launcher.
    hold = '500 key-down voice\n7500 key-up voice\n9000 pen-down 23 352\n9100 pen-up\n10500 pen-up\n'
    results = {}
    # At the ROM's full +70 dB (3162x), level 4 gives an ADC peak of about 12600, so levels 4
    # and 2 stay under the clip.
    for level in (4, 2, 200):
        results[f'level-{level}'] = record(args, f'level-{level}', level, hold)
    results['silent'] = record(args, 'silent', 0, hold)
    results['save-during'] = record(args, 'save-during', 4, hold, save_at=5)
    results['load-saved'] = load_saved(args, 'load-saved', args.output / 'save-during' / 'during' / 'during.frond')
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')

    failures = []
    def check(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            failures.append(what)
    for name, r in results.items():
        print(name, r['tag'], r['channels'], r['rate'], f"{r['seconds']:.2f}s",
              [(w['start'], w['rms'], w['frequency']) for w in r['windows']])
    full, half = tone(results['level-4'], .5, 3), tone(results['level-2'], .5, 3)
    check(len(full) >= 4 and len(half) >= 4, 'level 4 and 2 memos are over 3 s')
    for name in ('level-4', 'level-2', 'level-200', 'save-during'):
        check(all(abs(w['frequency'] - 1000) < 20 and w['tone_rms'] > .9 * w['rms'] for w in tone(results[name], .5, 3)),
              f'{name} is a 1 kHz tone')
    ratio = sum(w['rms'] for w in full) / max(1e-9, sum(w['rms'] for w in half))
    # At levels this small the fake source's rounding sets the ratio: lround(level * sin) in
    # 45 degree steps is 0, 1, 2, 1... at level 2 and 0, 3, 4, 3... at level 4, about 2.38.
    expected = source_rms(4) / source_rms(2)
    print(f'RMS ratio level 4 / level 2: {ratio:.4f}, source {expected:.4f}')
    check(abs(ratio / expected - 1) < .04, 'the memo keeps the source ratio for half the input')
    loud = tone(results['level-200'], 1, 3)
    check(all(w['peak'] > 16000 for w in loud), 'level 200 is held near the ALC target')
    check(all(w['rms'] < 50 for w in results['silent']['windows']), 'a silent source records silence')
    during = tone(results['save-during'], .5, 3.5)
    check(len(during) >= 6 and max(w['rms'] for w in during) < 1.2 * min(w['rms'] for w in during),
          'saving during a recording leaves no gap or level step')
    loaded = tone(results['load-saved'], .5, 1)
    check(bool(loaded) and all(abs(w['frequency'] - 1000) < 20 for w in loaded),
          'the session saved mid-recording loads and its memo holds the tone')
    if failures:
        sys.exit(f'{len(failures)} check(s) failed')


if __name__ == '__main__':
    main()
