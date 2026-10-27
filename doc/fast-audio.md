# Experimental TX fast audio

Run a TX session with:

```sh
./out/frond /path/to/tx.session --experimental-fast-audio
```

The option is off by default. It currently requires the supplied TX 5.4 NOR
(MD5 `85b048a692aa317763a7077ea72da6cb`) and a recognised runtime sound driver.
Firmware and application files are read unchanged.

The normal guest audio path holds roughly 108 ms of already rendered SunVox
samples before DMA reaches the codec. This mode reads a stream at HALSoundWrite
and sends its PCM to SDL earlier. It applies the driver's per-stream volume and
pan gains, including its signed Q11 rounding. The late codec copy is disconnected
from the host queue while the preview is active. Guest calls, FIFO, DMA and codec
processing continue.

This mode supports one open 44.1 kHz signed 16-bit stereo channel, with
writes up to 4096 bytes. It checks the driver's code signatures, channel table,
format and gains before activating. Multiple streams, other formats, device-wide
controls, unreadable data, excessive queue depth or a host playback underrun
return to normal audio until
reset or restore. Closing the stream allows a later stream to activate again.
Fallback includes a 250 ms guest-time mute to drain the delayed hardware copy.

The frontend uses 120 Hz CPU slices and pauses guest execution when the preview
queue exceeds 3072 frames, resuming below 2048. It adds 512 silent frames before
the first audible block and rejects a queue above 6144 frames. The host audio
clock therefore controls sustained preview pacing. This is an experimental
compatibility compromise: guest timing is affected while waiting for audio.

Save states contain guest state only. Power sleep, reset or restore discards queued preview
samples and rechecks the driver on the next write. Supply the flag again when
starting a saved session in a new process. Audio off/on clears preview buffering.
SDL re-primes after queue replacement; already delivered backend audio cannot
be revoked. See the [lifecycle checks](session-testing.md#audio-card-and-host-io-lifecycle)
for sleep, restore and failure coverage.

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
An ARM interception test checks opt-in behaviour, suppression of duplicate
codec PCM and clearing preview state on save/restore and reset.

Timing instrumentation stays outside production builds. Emulator callback
measurements exclude the host sound server and physical output device; they do
not establish physical-TX latency. Firmware-containing sessions and RAM exports
must stay outside Git.
