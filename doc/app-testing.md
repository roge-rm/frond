# Testing Palm applications

Use your own firmware and application files. Start the Palm and wait for its
application event loop before installing. From the desktop, use **File → Install
Application** and **Launch Application**. From the native console:

```text
install /path/to/application.prc
launch ApplicationDatabaseName
```

The launch name is the Palm database name, including case, which may differ from
the filename. If installation reports `not currently possible`, let startup
finish and retry. A matched PACE signature does not mean the event loop is ready.

Save a session to retain installed applications and the complete running state.
`save-nand /path/to/new-working-nand.bin` exports flash alone; use a new filename
and verify it with a cold boot if you need a reusable flash image.

## Application smoke tests

SimCity exercises the 68k interpreter, stylus input and drawing. Install it,
launch it, create a city and place a zone. Check that the simulation advances
and that menu and construction taps reach the expected controls.

SunVox exercises a 68k launcher and native ARM synthesis. Install it, launch
`SunVox`, choose a colour theme and check that the editor shows Generator, Echo
and Output. Select Generator and play two keys an octave apart. Inspect the
keyboard position before replaying coordinates; opening Palm's input area moves
controls.

For sustained playback, install the shareable
[project fixtures](../tests/fixtures/README.md). Use the Echo project for
continuous sound and the dry project for isolated key onsets. Test Play, Stop,
replay and menus as well as startup. Record the application version, firmware,
settings and session used so comparisons run the same workload.

## PCM capture and continuity

`tools/check-native-audio.py` captures the emulator's own PulseAudio stream,
records SDL underruns and host load, and keeps replay screenshots in a new result
directory. Install `pactl` and `parec`; PipeWire's PulseAudio compatibility server
also works.

```sh
SDL_AUDIODRIVER=pulseaudio python3 tools/check-native-audio.py \
  /path/to/playing-tx.session /path/to/new-results \
  --binary /path/to/frozen-frond --seconds 180 --continuous \
  --replay tests/fixtures/playing-ui.replay
```

Use `--continuous` only with a known continuous fixture. It requires nonzero,
unclipped PCM, no SDL underruns and no all-zero 10 ms blocks after three seconds
of warm-up. Musical rests are not emulator faults. Capture starts after the
runner identifies the SDL stream, while underruns are counted across the run.

Inspect the screenshots to confirm that menus responded; input dispatch alone
does not establish that an app handled a tap. Listen to the recording too. A
monitor capture excludes physical output-device latency. A low SDL underrun
count alone cannot establish continuity: a guest can supply silent buffers.

Keep firmware-containing sessions, RAM exports and recordings out of Git. RAM
exports contain all installed RAM databases, including unrelated user data.
Share an individual app log rather than the whole archive.

## Native ARM sound applications

For an application that exposes Play, Stop and Bench, exercise playback after
Stop and after Bench, including the case where Stop leaves a stream open for
release tails. Export its log after capture has finished so export work does
not contaminate the playback interval.

`tests/fixtures/acidulite-controls.replay` provides this sequence for Acidulite's
expanded-input-area layout. Use `--seconds 60 --export-databases` with the audio
runner, without `--continuous`. Confirm the button coordinates before using a
different layout or app version. Guest-reported Bench timings are emulator
measurements, not physical Palm timings.

## Automated checks

`make test` covers CPU alignment, PACE signature validation, device state,
audio queues, export rollback and the model regressions. `make test-desktop`
checks the desktop client protocol and save-failure workflow. Optional
firmware-backed desktop, SD and NetLib tests are described in the
[desktop](desktop.md) and [peripheral](peripherals.md) guides.

For fixed-budget CPU/PCM comparison, see
[native performance tools](tx-native.md#native-performance). For keyboard and
menu timing, see [input latency](input-latency.md).
