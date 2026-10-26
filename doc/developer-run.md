# Run a Palm TX application

`tools/frond-run.py` installs PRC/PDB files, launches an app, runs input replay,
captures screenshots and exports named Palm databases. It uses a private copy
of your session and saves the resulting state as `working.frond`. You need your
own TX session with Palm OS setup completed. Firmware and apps are not included.

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

Install flags are processed in order and may be repeated. The app argument is
the exact Palm database name, including case, rather than its PRC filename.
An installation that requests a reset triggers a reset and another readiness
check before the next install. Other install errors stop the run.

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
The runner separately waits for `guest-ready`, which prepares Palm OS's
`EvtGetEvent` boundary for application commands. A PACE signature alone is not
sufficient. It repeats this check after resets and before replay.

Readiness describes the instant it is checked; the guest can leave that boundary
before the next command. Database export therefore performs its own preflight
immediately before checkpointing. Live guest progress during preflight persists
on both success and failure. Injected export work rolls back to the resulting
checkpoint. Named exports retry explicit `not-ready` responses within
`--ready-timeout`; other export errors fail immediately.

An idle third-party app using `EvtGetEvent(evtWaitForever)` can remain not-ready
until normal user input occurs. Safe injection inside that blocked native call
is an [open bridge issue](tx-native.md#open-developer-workflow-issue-indefinitely-idle-applications).
The SD test probe's bounded idle wait is a fixture workaround; the runner does
not send input or modify apps to manufacture readiness.

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

Artifacts include the session, core log, screenshots with their requested times
and exports with their database names. Each `--export-db NAME` writes a ZIP named
`database-NNN.zip` containing only that database, useful for application logs.
A missing or unexportable database fails the run. No full RAM export is needed.
`session_saved` is true only after a successful final save. On failure,
`working.frond` may still be the starting session; completed artifacts and the
core log remain for inspection. Replay dispatch errors are recorded in `core.log`;
the JSON reports the closed socket or process exit and the replay stage.

The output directory must not already exist. It is created with mode 0700;
session and install copies have mode 0600. The core receives the copied session
and copied PRC/PDB files. Original input sessions, firmware and app files are
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
