# Run a Palm TX application

`tools/frond-run.py` installs PRC/PDB files, launches an app, runs input replay,
captures screenshots and exports named Palm databases. It uses a private copy
of your session and saves the resulting state as `working.frond`. You need your
own TX session or flash images with Palm OS setup completed. Firmware and apps
are not included.

Build the core, then choose a new result directory outside Git:

```sh
heavy build make -j4 out/frond
python3 tools/frond-run.py \
  --session /private/tx-ready.session \
  --install /private/MyApp.prc --install /private/TestData.pdb \
  --app 'My App' --replay /private/test.replay \
  --screenshot 0 --screenshot 2000 --screenshot 5000 \
  --export-db MyAppLog --run-for 6 --headless \
  --output /private/results/my-app-run-01
```

Python 3.11 or later is required. `--core /absolute/path/to/frond` selects another
core. When `heavy` is on PATH, the runner automatically uses `heavy emulator` and
holds that shared slot until the core exits. Run builds separately; do not wrap
the runner in another `heavy emulator` command.

For a cold boot, replace `--session` with `--nor /private/tx-nor.bin --nand
/private/tx-nand.bin`. These must be a 16 MiB NOR and a 132 MiB raw NAND image,
including spare bytes. This starts without saved RAM or CPU state. Prepared flash
can already contain Palm setup and applications; cold boot does not imply factory
setup coverage. `--resume-app` omits an explicit application launch in either mode.

Install flags are processed in order and may be repeated. The app argument is
the exact Palm database name, including case, rather than its PRC filename.
An installation that requests a reset triggers a reset and another readiness
check before the next install. Other install errors stop the run.

Use `--resume-app` instead of `--app NAME` to continue the saved application's
current screen without launching it again. This matters for games that restart
their intro or show an About dialog on each launch. The optional settle still
runs. Avoid installing applications in this mode when the
saved foreground state matters: an installation or reset can change that state.

`--headless` uses SDL's dummy display and audio drivers. Omit it to see the Palm
window. Sound is off unless you pass `--sound`. The runner inherits the rest of
your environment, including SDL driver settings.

## Replay and screenshots

Replay times and `--screenshot MS` use milliseconds relative to the replay start,
after launch and `--settle` seconds (default 1). For example:

```text
100 pen-down 100 140
200 pen-up
1500 key-down center
1600 key-up center
2000 snapshot "any-original-name.png"
```

The runner copies the replay into its result directory. Every replay `snapshot`
destination is replaced with `replay-snapshot-NNN.png` in that directory, including
absolute destinations. Requested screenshots become `screenshot-NNN.png`.
Original replay files and snapshot destinations stay untouched. Equal-time events
keep their order; extra screenshots at that time follow the replay's events.
Screenshots contain the Palm framebuffer without the desktop button strip.

The run waits for the core to finish dispatching the entire replay and writing
its screenshots. `--run-for SECONDS` sets a minimum replay interval; later events
or screenshots extend it. Without a replay or screenshots it simply waits that
interval. Native replay validation still checks coordinates, key names and event
arguments. A screenshot failure stops the core and fails the run.

Replay completion confirms input dispatch, not that the app handled each event.
Inspect screenshots and the exported app log to check the result. Readiness also
does not establish that an app's first screen has finished drawing; increase
`--settle` or put a delay before the first replay event when needed.

## Readiness and time limits

The desktop socket's `ready` event means that the host control channel is open.
The runner separately waits for `guest-ready`, which finds a supported Palm OS
application-call boundary. The qualified TX native path also supports an idle
native app without waiting for a 68k event trap. A PACE signature alone is not
sufficient. It repeats this check after resets. Replay itself does not require
an application-call boundary; resuming without installs also skips the initial
readiness check.

Readiness describes the instant it is checked; the guest can leave that boundary
before the next command. Database export therefore performs its own preflight
immediately before checkpointing. Live guest progress during preflight persists
on both success and failure. Injected export work rolls back to the resulting
checkpoint. Named exports retry explicit `not-ready` responses within
`--ready-timeout`; other export errors fail immediately.

The [qualified TX native boundary](tx-native.md#pace-and-palm-api-injection)
allows operations during native event waits without sending input or modifying
apps. Unsupported firmware, busy guest code or an unqualified task context can
still return `not-ready`. Readiness and command deadlines remain bounded.
Failed installations roll back guest changes, including a failed replacement
that has already deleted the old database. A successful install or launch
commits its changes; database export always rolls back its injected guest work.

Only `not-ready` errors are retried. A missing app, corrupt database, an open app
that cannot be replaced, failed export or unknown protocol command fails
immediately. Use a session with the app closed when replacing it. A core without
the new readiness command reports `command`; rebuild it.

All limits are wall-clock seconds and accept fractional values:

| Option | Default | Scope |
| --- | ---: | --- |
| `--timeout` | 180 | Overall budget, including input preparation, slot wait and shutdown |
| `--startup-timeout` | 60 | Waiting for a shared slot, socket connection and host ready event |
| `--ready-timeout` | 30 | Each readiness phase or retryable install/launch phase |
| `--command-timeout` | 30 | Each socket command, including exports and saves |

The earliest applicable limit wins. Replay has the overall deadline; its status
requests also have command deadlines. Large replays need a larger `--timeout`.
The native core bounds ordinary calls to 50 million cycles. Database imports may
continue the same call through up to ten 50-million-cycle slices, with deadline
checks between unfinished slices. The CPU rate remains 50 million cycles per
accounted guest second. A native operation session is capped at 20 accounted guest
seconds and 30 wall seconds, checked between calls/callbacks and slices. Raising runner
deadlines does not raise those core bounds. Native exports allow up to 64 MiB per
database and 256 MiB per archive; exceeding a bound fails without publishing a ZIP.
On timeout or Ctrl-C, the runner stops only its own process group, including a
queued `heavy` invocation. Cleanup allows up to three seconds before forcing
that group to exit. It does not attempt a save after a failed command or timeout.
File copies and hashing finish their current filesystem operation before checking
the overall deadline.

## Results and privacy

The runner prints one JSON object to stdout and writes the same object to
`result.json`. Core output, including shared-slot wait messages, goes to
`core.log`. Exit status is 0 for success and 1 for a failed run. Invalid CLI
syntax uses argparse's usual usage message and exit status 2.

The JSON has `version: 1`, `status` (`ok` or `error`), `stage`, `error` (a code
and message, or null), ordered socket `steps`, input paths and SHA-256 hashes,
`elapsed_seconds`, `core_exit_code` and `artifacts`. Each step includes command,
argument, success, code and message. The stage identifies validation, preparation,
startup, readiness, install, launch, replay, export, save, quit or completion.
`origin` distinguishes `cold-boot` from `session-restore` and records whether
saved RAM/CPU state was loaded. `inputs_unchanged` verifies the original input
hashes again after execution; a changed original makes the run fail.

Artifacts include the session, core log, screenshots with their requested times
and exports with their database names. Each `--export-db NAME` writes a ZIP named
`database-NNN.zip` containing only that database, useful for application logs.
A missing or unexportable database fails the run. No full RAM export is needed.
Archive entry filenames escape path separators, percent signs and control
characters; the database name in the PRC/PDB header remains unchanged.
`session_saved` is true only after a successful final save. On failure,
`working.frond` may still be the starting session; completed artifacts and the
core log remain for inspection. Replay dispatch errors are recorded in `core.log`;
the JSON reports the closed socket or process exit and the replay stage.

The output directory must not already exist. It is created with mode 0700;
session, flash and install copies have mode 0600. The executable is also frozen
in the output directory and run from that copy. The core receives copied session
or flash and PRC/PDB files. Original input sessions, firmware and app files are
never save destinations. Keep the whole result directory private: sessions
contain firmware and personal data, and logs, screenshots and database exports
can contain user data. Do not commit them.

## Tests

```sh
heavy build make test-developer-run
python3 tests/desktop-control.py --core out/frond --session /private/tx-ready.session
```

The first command uses a synthetic socket peer and needs no firmware. It covers
ordered installs, resets, readiness retries, replay completion, snapshot
redirection, named exports, input preservation, protocol errors and bounded
startup, command and overall waits. The optional second command exercises the
real TX socket, readiness, replay status, PNG capture, save and restore using
your session. It automatically takes a shared emulator slot.

For a separate real-guest workflow check, build and run the optional idle probe:

```sh
python3 tools/build-palm-probe.py --probe idle
python3 tests/desktop-idle.py --session /private/tx-ready.session --output /private/results/idle-check
```

The probe stores wait counters in RAM so the driver can distinguish an
outstanding event call from periodic nil returns. It refuses to count a
nil-returning control as blocked-wait acceptance. The native case additionally
requires independently verified kernel-idle ranges for that firmware; see
`python3 tests/desktop-idle.py --help`. Both cases retain private checkpoints,
request logs and original hashes, and exercise replacement failure, named ZIP
exports, launch, persistence and controlled log retrieval without key or pen
input. `--baseline` reproduces readiness failures and always exits nonzero.
