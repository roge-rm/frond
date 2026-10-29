# Experimental TX and Tungsten E2 fast audio

Run a qualified TX or Tungsten E2 session with:

```sh
./out/frond /path/to/tx.session --experimental-fast-audio
```

The option is off by default. It requires a qualified TX 5.4 or Tungsten E2 NOR
and a recognised runtime sound driver. Other firmware is refused before startup.
Firmware and application files are read unchanged.

The preview reads completed PCM before the firmware's blocking output queue.
For a qualified four-byte-aligned ARM basic callback, it can read the accepted
render-to-writer handoff before the writer wakes. That earlier point requires a
successful stream start observed in the current host timeline, positive queue
and message ownership, and a matching HAL handoff. Variable-length callbacks,
decoder wrappers and streams restored mid-flight use the existing HALSoundWrite
interception. Reopening the application can establish a new qualified start.

Both points apply the driver's volume and pan gains, including signed Q11
rounding. E2 has a separate guarded driver profile. Guest calls, callback
execution, queues, mixing, DMA and codec processing continue. The delayed codec
copy is disconnected from the host queue while preview is active, and a buffer
already accepted by the earlier preview is not enqueued again at HAL.

Support is limited to one open 44.1 kHz signed 16-bit stereo channel and writes
up to 4096 bytes. Driver signatures, format, rates, gains and ownership are
checked before earlier output. Multiple streams, unsupported formats or
controls, unreadable data, excessive queue depth and host playback starvation
latch a return to normal audio until reset or intentional restore. An ordinary
stream close without a latched failure permits a later supported stream to
activate again.

Affecting controls immediately discard undelivered host PCM. Stop keeps output
suspended until a new successful start. Suppression records remain until the
guest consumes or disposes the corresponding message. During fallback, the
250 ms guest-time drain interval cannot expire while an already-previewed
message is pending, and restarts at that message's later retirement. If ownership
cannot be established after corrupted guest state, suppression persists until
retirement, joined close, reset or restore establishes a safe boundary.

The frontend uses 120 Hz CPU slices and pauses guest execution when the preview
queue exceeds 3072 frames, resuming below 2048. It adds 512 silent frames before
the first audible block and rejects a queue above 6144 frames. The host audio
clock therefore controls sustained preview pacing. This is an experimental
compatibility compromise: guest timing is affected while waiting for audio.

Save states contain guest state only. Power sleep, reset and intentional restore
discard queued host PCM. Audio off/on cancels obsolete delivery while retaining
suppression history. Temporary guest work for database operations is fenced
before execution: rollback preserves message identities and control scopes;
commit releases the fence without replaying temporary audio. A rejected load
preserves the original timeline's suppression records.

SDL re-primes after queue replacement. Already delivered backend samples cannot
be revoked, including an earlier tail heard before a later stop. Supply the flag
again when starting a saved session in a new process. See the
[lifecycle checks](session-testing.md#audio-card-and-host-io-lifecycle).

## Testing and limits

Use the [keyboard timing protocol](input-latency.md#keyboard-onset) with
`--experimental-fast-audio` to compare against the normal path. Check both note
onsets and sustained playback. A shorter queue can reduce response time while
making playback more sensitive to host scheduling; there is no guaranteed
end-to-end latency target.

The SDL underrun counter triggers a return to normal audio after starvation.
That decision is latched until reset or restore. Fallback includes the guest-time
drain mute described above, followed by normal buffering, so a brief gap is
expected. Use normal audio for projects that cannot sustain the smaller queue.

`make test` covers the driver profile, PCM gains, silence, disabled output,
unsupported streams, invalid PCM addresses, queue overflow, close and reset.
`test-earlier-audio` also checks ordered retirement, cancellation, delayed HAL
fallback, control returns, checkpoint identity, callback eligibility, and bounded
patch-dispatch tails with corrupt-state refusal.
ARM interception tests on both boards check opt-in behaviour, suppression of duplicate
codec PCM and clearing preview state on restore and reset.

Timing instrumentation stays outside production builds. Emulator callback
measurements exclude the host sound server and physical output device; they do
not establish physical-TX latency. Firmware-containing sessions and RAM exports
must stay outside Git.

With a local Palm OS SDK, build the original guest acceptance application with
`python3 tools/build-palm-probe.py --probe audio --sdk /path/to/sdk-5r3`.
It writes `FrondAudioLog` while exercising basic and variable-length callbacks,
pause/resume, stop/restart, volume/pan, delete/reopen and simultaneous streams.
Export that database and run `python3 tools/check-audio-probe.py /private/log.zip`.
The checker validates callback patterns and guest API results. Host delivery,
cancellation timing and playback continuity require separate captures.
