#!/usr/bin/env python3
"""Zire 72 Camera records a video clip of the core's test pattern and the fake microphone through
the real ROM, and Media plays it back.

run --core CORE --session SESSION --output DIR
    opens Camera with a blank SD card, switches to video, records about 5 s at 320 x 240, saves
    the clip and plays it in Media. Then exports the card and decodes the clip: every frame is
    checked against the pattern, its counter has to go up one a frame at about the sensor's frame
    rate, and the audio track has to carry the 1 kHz tone at the level the ROM's gain gives. Then
    saves a session in the middle of a second recording, checks that clip, loads the saved session
    in a new process, stops the recording there and checks that clip too. CORE is the core built
    with the fake microphone (test-z72-voice-memo's frond-test-mic), run with --emulated-time so
    replay times and the guest's speed are the same on any host. SESSION is a Zire 72 session
    at the launcher with the setup Reminder up (the smoke session); private, so it's never copied
    anywhere else.

Needs ffmpeg and mtools.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

here = Path(__file__).parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, here / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


voice_memo = load('voice_memo', 'z72-voice-memo.py')
camera = load('camera', 'z72-camera.py')
Core = voice_memo.Core

# The fake microphone's peak, in ADC counts before any gain. Level 2 stays well under the clip.
LEVEL = 2
# Voice Memo's full codec gain is +70 dB (record boost and mic boost, 20 dB each, and the ALC at
# its +30 dB ceiling, the tone being under the ALC target); the video clip comes out a further
# 12 dB (4x) up, which the ROM adds itself.
GAIN = 10 ** (70 / 20) * 4
# OV9640 QVGA in the ROM's video setup: 26 MHz MCLK, CLKRC 0x83 (doubled, then divided by 8) for
# a 6.5 MHz pixel clock, and 400000 clocks a frame, so 16.25 frames a second. The clip declares 15.
SENSOR_FPS = 26e6 * 2 / 8 / 400000

OPEN = '500 pen-down 160 135\n600 pen-up\n6000 pen-up\n6001 snapshot "S/viewfinder.png"\n'
VIDEO = '500 pen-down 288 258\n600 pen-up\n4000 pen-up\n4001 snapshot "S/video-mode.png"\n'
SHUTTER = '500 pen-down 255 282\n600 pen-up\n'
# Shutter starts the recording, a second tap 5.5 s later stops it.
RECORD = (SHUTTER + '3000 snapshot "S/recording.png"\n6000 pen-down 255 282\n6100 pen-up\n'
          '9000 pen-up\n9001 snapshot "S/stopped.png"\n')
# The same, holding the guest 2.5 s into the recording for a save.
RECORD_SAVE = RECORD.replace('"S/recording.png"\n', '"S/recording.png"\n3000 pause\n')
SAVE = '500 pen-down 235 268\n600 pen-up\n5000 pen-up\n5001 snapshot "S/saved.png"\n'
HOME = camera.HOME
# Media from the launcher, then its first thumbnail, which plays the clip at once.
MEDIA = '500 pen-down 252 280\n600 pen-up\n6000 pen-up\n6001 snapshot "S/media.png"\n'
PLAY = ('500 pen-down 52 92\n600 pen-up\n' +
        ''.join(f'{t} snapshot "S/play-{i}.png"\n' for i, t in enumerate((2000, 3000, 4000, 5000))) +
        '9000 pen-up\n9001 snapshot "S/played.png"\n')


def blank_card(path):
    with open(path, 'wb') as f:
        f.truncate(64 << 20)
    subprocess.run(['mformat', '-i', str(path), '-v', 'FROND', '::'], check=True)


def export_clips(core, work):
    """Ejects the card and exports it; the core writes the image only on export."""
    for command, argument in (('sd-eject', ''), ('sd-export', str(work / 'out.img'))):
        ok, code, message = core.request(command, argument)
        assert ok, (command, code, message)
    listing = subprocess.run(['mdir', '-b', '-i', str(work / 'out.img'), '::/DCIM'], check=True,
                             capture_output=True, text=True).stdout.split()
    clips = sorted(p for p in listing if p.lower().endswith('.asf'))
    for clip in clips:
        subprocess.run(['mcopy', '-o', '-i', str(work / 'out.img'), clip, str(work)], check=True)
    return [work / Path(c).name for c in clips]


def probe(clip):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                          'stream=codec_type,codec_name,width,height,r_frame_rate,sample_rate,channels',
                          '-of', 'json', str(clip)], check=True, capture_output=True, text=True).stdout
    return json.loads(out)['streams']


def check_video(clip, work):
    """Decodes every frame: the counter it shows, its timestamp, and its pixels against the
    pattern on every fifth frame (the full compare is slow in Python)."""
    w, h = 320, 240
    times = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v', '-show_entries', 'frame=pts_time',
                            '-of', 'csv=p=0', str(clip)], check=True, capture_output=True, text=True).stdout.split()
    rgb = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(clip), '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         check=True, capture_output=True).stdout
    n = len(rgb) // (w * h * 3)
    frames, worst, unread = [], {'mean': 0, 'strict_over_24': 0}, 0
    for i in range(n):
        frame = rgb[i * w * h * 3:(i + 1) * w * h * 3]
        try:
            counter = camera.read_frame(w, h, frame)
        except AssertionError:
            unread += 1
            frames.append(None)
            continue
        frames.append(counter)
        if i % 5 == 0:
            r = camera.compare(w, h, frame, counter, edge=2)
            if r['mean'] >= worst['mean']:
                worst = dict(r, index=i)
    read = [f for f in frames if f is not None]
    steps = [b - a for a, b in zip(read, read[1:])]
    # The encoder settles in its first second; after it, every step.
    settled = [b - a for a, b, t in zip(read, read[1:], times[1:]) if float(t) - float(times[0]) > 1]
    span = float(times[-1]) - float(times[0]) if len(times) > 1 else 0
    return {'frames': n, 'unreadable': unread, 'first': read[0] if read else None, 'last': read[-1] if read else None,
            'steps': sorted(set(steps)), 'settled_steps': {k: settled.count(k) for k in sorted(set(settled))},
            'counters': frames, 'span': round(span, 3),
            'fps': round((n - 1) / span, 2) if span else 0,
            'counter_fps': round((read[-1] - read[0]) / span, 2) if span and read else 0, 'worst': worst}


def check_audio(clip, work):
    wave = work / (clip.stem + '.wav')
    subprocess.run(['ffmpeg', '-v', 'error', '-i', str(clip), '-vn', '-acodec', 'pcm_s16le', '-y', str(wave)], check=True)
    return voice_memo.analyse(wave)


def analyse_clip(clip, work):
    streams = probe(clip)
    video = next(s for s in streams if s['codec_type'] == 'video')
    audio = next(s for s in streams if s['codec_type'] == 'audio')
    return {'clip': clip.name, 'video': video, 'audio': audio, 'frames': check_video(clip, work),
            'sound': check_audio(clip, work)}


def start(args, name, source, card=True):
    work = args.output / name
    machine = work / 'machine'
    machine.mkdir(parents=True)
    shutil.copy(source, machine / 'machine.frond')
    os.chmod(machine / 'machine.frond', 0o600)
    # A saved session keeps its card in sd.frondsd beside it.
    for card in source.parent.glob('*.frondsd'):
        shutil.copy(card, machine / card.name)
    # Emulated time runs the Zire at its full 150 MIPS of guest time, slower than real time on most
    # hosts, so the waits get longer.
    core = Core(args.core, machine, LEVEL, work / 'core.log', ['--emulated-time'], patience=10)
    core.wait_ready()
    return core, work


def open_video(core, work):
    camera.run_replay(core, work, 'dismiss', camera.DISMISS)
    core.wait_ready()
    camera.run_replay(core, work, 'open', OPEN)
    # Inserted with Camera up; from the launcher it would switch the launcher to the card.
    blank_card(work / 'card.img')
    ok, code, message = core.request('sd-insert', str(work / 'card.img'))
    assert ok, (code, message)
    camera.run_replay(core, work, 'video', VIDEO)


def faults(work):
    text = (work / 'core.log').read_text(errors='replace')
    return [line for line in text.splitlines()
            if any(k in line for k in ('Invalid instr', 'Fatal', 'fault', 'Abort'))][:5]


def playback(work):
    """Media's playback view: the clip's pattern at 0,30, 320 x 240 (the film border above it).
    Returns the counters read from the screenshots."""
    out = []
    for i in range(4):
        w, h, rgb = camera.decode(work / f'play-{i}.png')
        try:
            out.append(camera.read_frame(320, 240, rgb, y0=30, stride=w))
        except AssertionError as e:
            out.append(str(e))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=['run'])
    parser.add_argument('--core', required=True)
    parser.add_argument('--session', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.core = str(Path(args.core).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    os.chmod(args.output, 0o700)
    results = {}

    core, work = start(args, 'record', args.session)
    try:
        open_video(core, work)
        camera.run_replay(core, work, 'record', RECORD)
        camera.run_replay(core, work, 'save', SAVE)
        camera.run_replay(core, work, 'home', HOME)
        camera.run_replay(core, work, 'media', MEDIA)
        camera.run_replay(core, work, 'play', PLAY)
        clips = export_clips(core, work)
    finally:
        core.quit()
    assert len(clips) == 1, clips
    results['record'] = analyse_clip(clips[0], work)
    results['record']['playback'] = playback(work)
    results['record']['faults'] = faults(work)

    # A save 2.5 s into a recording, then the recording goes on to its end in the same process.
    core, work = start(args, 'save-during', args.session)
    try:
        open_video(core, work)
        replay = work / 'record.replay'
        replay.write_text(RECORD_SAVE.replace('S/', str(work) + '/'))
        ok, code, message = core.request('input-replay', str(replay))
        assert ok, (code, message)
        deadline = time.monotonic() + 600
        while core.request('replay-state')[1] != 'paused':
            assert time.monotonic() < deadline, 'replay never paused'
            time.sleep(.2)
        ok, code, message = core.request('save', str(work / 'during.frond'))
        assert ok, (code, message)
        ok, code, message = core.request('replay-continue')
        assert ok, (code, message)
        while core.request('replay-state')[1] != 'finished':
            assert time.monotonic() < deadline, 'replay never finished'
            time.sleep(.2)
        camera.run_replay(core, work, 'save', SAVE)
        camera.run_replay(core, work, 'home', HOME)
        clips = export_clips(core, work)
    finally:
        core.quit()
    assert len(clips) == 1, clips
    results['save-during'] = analyse_clip(clips[0], work)
    results['save-during']['faults'] = faults(work)

    # The saved session in a new process: still recording; stop it there and save the clip.
    core, work = start(args, 'load-saved', args.output / 'save-during' / 'during' / 'during.frond')
    try:
        camera.run_replay(core, work, 'loaded', '1500 pen-up\n1501 snapshot "S/loaded.png"\n')
        camera.run_replay(core, work, 'stop', SHUTTER + '3000 pen-up\n3001 snapshot "S/stopped.png"\n')
        camera.run_replay(core, work, 'save', SAVE)
        camera.run_replay(core, work, 'home', HOME)
        clips = export_clips(core, work)
    finally:
        core.quit()
    results['load-saved'] = {'clips': [c.name for c in clips]}
    if clips:
        results['load-saved'].update(analyse_clip(clips[-1], work))
    results['load-saved']['faults'] = faults(work)
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=1))

    failures = []

    def check(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            failures.append(what)

    expected = voice_memo.source_rms(LEVEL) * GAIN
    print(f'expected tone RMS {expected:.0f}, sensor {SENSOR_FPS:.2f} frames a second')

    def check_clip(name, r, seconds, phase_jumps=0):
        v, a, f, s = r['video'], r['audio'], r['frames'], r['sound']
        check((v['width'], v['height']) == (320, 240), f'{name}: the clip is 320 x 240')
        check(a['sample_rate'] == '8000' and a['channels'] == 1, f'{name}: the audio is 8 kHz mono')
        check(f['frames'] >= seconds * 13 and f['unreadable'] == 0, f'{name}: {f["frames"]} frames, all with a counter')
        # The clip is stamped at about 15 frames a second and takes the newest sensor frame each
        # time, so after the first second the counter goes up one a frame, two about one frame
        # in sixteen, at the sensor's rate.
        settled = f['settled_steps']
        check(set(settled) <= {1, 2} and settled.get(1, 0) >= .85 * sum(settled.values()),
              f'{name}: the counter goes up one a frame, now and then two {settled}')
        check(abs(f['fps'] - 15) < 1, f'{name}: {f["fps"]} frames a second')
        check(abs(f['counter_fps'] - SENSOR_FPS) < 1, f'{name}: {f["counter_fps"]} sensor frames a second')
        # MPEG-4 at the ROM's bit rate blurs edges and the counter digits a little.
        check(f['worst'].get('mean', 999) <= 12 and f['worst'].get('strict_over_24', 1) <= .01 * f['worst'].get('strict_pixels', 0),
              f'{name}: the frames match the pattern')
        tone = [w for w in s['windows'] if .5 <= w['start'] < s['seconds'] - .5]
        check(len(tone) >= 2 * (seconds - 1), f'{name}: {s["seconds"]:.1f} s of sound')
        # The fake source starts a new sine at a load, a phase jump that takes about a quarter
        # of one window's tone measure.
        check(all(abs(w['frequency'] - 1000) < 20 for w in tone) and
              sum(w['tone_rms'] <= .9 * w['rms'] for w in tone) <= phase_jumps,
              f'{name}: the sound is the 1 kHz tone')
        check(all(abs(20 * math.log10(max(1, w['rms']) / expected)) < 1 for w in tone),
              f'{name}: the tone is within 1 dB of {expected:.0f} RMS')
        check(not r['faults'], f'{name}: no faults in the core log')

    check_clip('record', results['record'], 5)
    play = results['record']['playback']
    check(all(isinstance(p, int) for p in play) and all(b > a for a, b in zip(play, play[1:])),
          'Media plays the clip, its counter going up')
    # The replay holds the guest for the save, so this clip is as long as the first.
    check_clip('save-during', results['save-during'], 5)
    loaded = results['load-saved']
    check(len(loaded['clips']) == 1 and 'frames' in loaded, 'the loaded session saves its recording')
    if 'frames' in loaded:
        check_clip('load-saved', loaded, 3, phase_jumps=1)
        # The clip starts with the frames recorded before the save.
        check(loaded['frames']['counters'][:15] == results['save-during']['frames']['counters'][:15],
              'the loaded clip starts with the frames from before the save')
    if failures:
        sys.exit(f'{len(failures)} check(s) failed')


if __name__ == '__main__':
    main()
