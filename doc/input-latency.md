# Measuring input response

Keyboard and menu checks use optional diagnostic patches. Production builds
contain no timing instrumentation. Use the same firmware, app version, saved
project and host conditions when comparing builds.

These tools measure emulator input enqueue to PCM callback or presentation.
They exclude the physical stylus, sound-server/device delay and display scanout.
A physical Palm comparison needs its own matched recording.

## Keyboard onset

Install `tests/fixtures/frond-latency.pdb`, launch SunVox and load
`A:/frond-latency.sunvox`. It connects Generator directly to Output, with Echo
disconnected and a short release so notes have a quiet baseline. Stop playback
and expand the Palm input area so the keyboard is at y=151. Inspect a screenshot
before running the replay, then save a session for repeatable tests.

Apply the optional trace to a prepared source tree:

```sh
git apply --directory=build/source tools/debug/input-latency-trace.patch
make -j4
FROND_LATENCY_PCM=out/keyboard.raw ./out/frond /path/to/idle.session \
  --run-for 27 --input-replay tests/fixtures/frond-latency.replay \
  > out/keyboard.log 2>&1
python3 tools/analyze-input-latency.py out/keyboard.log out/keyboard.raw --y 151
```

The trace records monotonic timestamps for enqueue, touchscreen delivery and SDL
callbacks, plus the PCM submitted to SDL. Onset is the first channel sample of
magnitude at least 100 after a quiet baseline. The analyser stops searching at
the next press, keeping missed taps from claiming a later response.

PCM time is callback start plus sample offset at 44100 Hz. Allow roughly one
callback of uncertainty. The loop tone detector also has windowing uncertainty;
it should not be compared directly with dry-note sample onset.

The trace writes from the audio callback and can affect scheduling. Compare
equivalent runs and remove it before normal use:

```sh
git apply -R --directory=build/source tools/debug/input-latency-trace.patch
make -j4
```

The [fast audio option](fast-audio.md) can be tested with the same protocol by
adding `--experimental-fast-audio` to the emulator command.

## Guest pipeline stages

`tools/debug/guest-audio-stages.patch` observes the Palm API bridge, SunVox render
callback returns and AC97 descriptor fetches. Apply it alongside the input trace,
rebuild and enable `FROND_GUEST_TRACE=1` as well as `FROND_LATENCY_PCM`.

```sh
python3 tools/analyze-guest-latency.py out/keyboard.log out/keyboard.raw
```

This diagnostic contains firmware- and SunVox-1.7.3c-specific addresses, including
fallback addresses for a particular saved layout. Review them before using a
different firmware, app or session. The analyser ignores silent render/DMA blocks
and stops attribution at the next press. Reverse the guest-stage patch before
removing the input trace, then rebuild.

## Menu response

`tools/debug/menu-response-trace.patch` measures pen enqueue, touchscreen delivery,
a new menu framebuffer and SDL presentation return. Apply it to a prepared tree,
rebuild and enable `FROND_MENU_TRACE=1`. Start with the SunVox menu closed and run
`tests/fixtures/menu-response.replay` for 26 seconds, then analyse the log:

```sh
python3 tools/analyze-menu-response.py /path/to/frond.log
```

The observer targets the 320-pixel-wide editor. A tap at `(274, 8)` opens the menu;
at least 900 light pixels in `(196..319, 180..239)` mark its lower portion as
visible. Check screenshots to confirm the expected UI. This criterion does not
prove that every menu pixel is drawn or that a second tap is accepted.

The analyser requires a new matching framebuffer followed by presentation and
retains missing responses. Reverse the patch and rebuild after use.

For a guest-time workload without SDL or the frontend governor:

```sh
make benchmark-menu
./build/benchmark-menu /path/to/playing-tx.session 60
```

The benchmark runs 24 guest seconds and reports CPU cost, guest latency and the
first qualifying region's hash. It expects the playing SunVox fixture with its
menu closed; arbitrary sessions can give misleading pixel matches. The argument
sets the virtual CPU budget, not a hardware-equivalent TX speed.

## Test conditions

Record host load and capture missing responses as failures. Optional
`tools/check-native-audio.py --cpu N` binds the emulation thread for comparisons,
but does not reserve a core or control CPU frequency. It is not a playback
default. Pair timing with PCM continuity checks, since a responsive menu alone
does not establish reliable sound.

The capture tool records each emulator thread's actual Linux scheduling policy,
priority and accumulated CPU time at startup and once per second. This also
shows whether a requested real-time policy was granted or later removed. The
`host_load_samples` entries include these readings alongside host load. Run-queue
wait time is recorded when kernel scheduler statistics are enabled; otherwise
it is `null`, since a zero counter would not establish that the thread never
waited. Sampling does not change scheduling policy or kernel settings.

PCM metrics report the longest internal silent run and trailing silence
separately. This helps distinguish a playback gap from silence captured during
shutdown. The strict continuity verdict still includes trailing silence; these
additional fields do not relax its checks.

`make test` covers onset detection, background rejection, missed-tap attribution,
guest-stage attribution and new-frame/presentation requirements. Sessions and
raw diagnostic exports stay outside Git.

## Audio clock and DMA diagnosis

Native audio backpressure pauses guest execution, including its timer, DMA and
codec clocks, until SDL drains the queue. Host input polling, drawing and desktop
control remain active. Pausing only the codec while guest time continues lets
sound-stream deadlines advance against a stopped sample clock. This correction
does not guarantee continuous PCM when the host cannot sustain the guest workload.

For a matched guest CPU ceiling, add `--mips 40` (for example) to
`tools/check-native-audio.py`. This is a ceiling, not a fixed budget: the native
governor can still select a lower rate under host load. Compare the recorded
`budget_mips_samples` as well as host load before attributing a difference to a
change. Keep the binary and session unchanged throughout each measurement.

`tools/debug/audio-timing-trace.patch` observes HAL writes, sampled DMA descriptor
contents, codec silence/FIFO starvation, sound-stream lifecycle and backpressure.
Apply this standalone observer to prepared source, rebuild, then enable
`FROND_HAL_AUDIO=1 FROND_GUEST_TRACE=1 FROND_PCM_TRACE=1`. Summarize a captured log:

```sh
python3 tools/analyze-audio-timing.py /path/to/frond.log
```

HAL writes are downstream of application callbacks; their intervals are not
callback execution times. DMA peaks inspect every eighth frame at descriptor
fetch, so an all-zero probe is not proof of an entirely silent transfer. Codec
rows cover 44,100 output frames and include startup/shutdown. Use lifecycle
markers or a saved active continuous stream to distinguish intentional silence.
A nonzero HAL buffer followed by silence in DMA/codec without FIFO starvation
localizes the fault upstream of SDL, but does not by itself identify its cause.

Backpressure intervals report both host time and guest time. Guest time should
stay fixed while the native frontend waits. The analyser separates lifecycle
boundaries and excludes negative intervals after a guest clock restore; it does
not issue a continuity verdict. Use the existing strict PCM acceptance alongside
it, retaining failed captures and shutdown tails.

The observer logs on the emulation thread and changes measured throughput. It
uses the existing TX HAL trap layout and is a diagnostic, not a general sound
API monitor. Run uninstrumented comparisons separately and reverse the observer
before ordinary builds. No host real-time scheduling changes are required.
