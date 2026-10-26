# SunVox playback fixture

`frond-pattern.sunvox` is a project created locally in SunVox 1.7.3c on the
emulated TX. It contains one 32-row, one-track pattern: C4, E4, G4, C5 repeated
eight times, using the default Generator → Echo → Output modules. Tempo is
125 BPM, six ticks per row; one loop should take 3.84 seconds. No external
samples, application binaries or firmware are included.

`frond-pattern.pdb` wraps exactly that project in one Palm database record
(type `PSYX`, creator `ZULU`, database name `frond-pattern.sunvox`). Install it
through the Palm launcher, then open SunVox → wrench menu → Load song →
`A:/frond-pattern.sunvox`. The standalone file is suitable for other SunVox
hosts or a device with an accessible filesystem.

For the physical TX comparison, loop for about three minutes and note audible
gaps, tempo drift, and whether stop/menu taps respond promptly. Use the same
SunVox version and default module settings. A recording is useful but optional.
Monitor captures alone cannot distinguish intentional song silence from a fault;
compare this known continuous pattern, guest-buffer tracing and device playback.

`frond-latency.sunvox` and its Palm wrapper `frond-latency.pdb` use the same
pattern with Output connected directly to Generator, Echo disconnected and
Generator release set to 32. Use this variant for keyboard onset timing without
an echo tail obscuring the next note. `A:/frond-latency.sunvox` is the Palm path.

`playing-ui.replay` exercises four higher notes and a menu while the Echo fixture
is already playing for three minutes. `acidulite-controls.replay` assumes a
stopped Acidulite form, expanded input area, and buttons at y=300; it exercises
Play/Stop/replay/Bench with snapshots. Coordinates depend on that saved UI state.
Use these through `tools/check-native-audio.py`, which keeps each run's snapshots
separate. The fixtures contain no firmware or application binary.

`menu-response.replay` opens the SunVox editor menu three times after warm-up,
with snapshots before each tap and after a 2.5-second response window. Start with
the menu closed and run for 26 seconds. Pair it with the optional menu observer
and analyser described in [input latency](../../doc/input-latency.md).
