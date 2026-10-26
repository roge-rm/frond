# Palm TX compatibility coverage

This is a repeatable application/service smoke matrix for the current TX model.
It separates synthetic interpreter results from Palm OS results obtained with
your own firmware. Passing these probes establishes the listed observations on
that build and session. It does not establish general application compatibility,
complete instruction or API coverage, timing accuracy, or hardware equivalence.
Physical TX comparisons are deferred while the device is unavailable.

## Firmware-free checks

```sh
heavy build make -j4 test-compatibility
python3 tools/check-compatibility.py --output /private/results/compatibility-01
```

`test-compatibility` is included in `make test`. It builds and runs synthetic
programs through the actual core, and tests the matrix/log validator. No Palm
SDK, firmware, PRC, network access or license acceptance is required.

The synthetic `pace-execution` case checks big-endian mixed-width memory,
arithmetic, a taken conditional branch, nested subroutine returns, balanced
stacks, an OS trap boundary, illegal instruction, division by zero and an unmapped
instruction fetch. It tests Frond's accelerated 68k interpreter, not the original
firmware interpreter or Palm OS trap implementation.

`native-interworking` executes an ARM → Thumb → ARM call chain and checks its
result, execution state, stack and preserved register. Existing alignment,
condition-code, cache and GDB tests provide additional instruction-level coverage.
These small programs are deliberately bounded; they are not ISA conformance tests.

The Python tests reject stale tokens, unfinished runs, duplicate or missing
results, guest failures, wrong values, bad record offsets and wrong database
identity. They also check session chaining and explicit skipped firmware rows.
Synthetic socket tests in `test-developer-run` separately check readiness, ordered
installs, input preservation, export failures, deadlines and process cleanup.

## Optional Palm OS service and native application probe

Use an already installed local Palm OS 5 SDK, PRC tools and ARM toolchain. The
builder follows the existing peripheral probe conventions and downloads nothing:

```sh
heavy build python3 tools/build-palm-probe.py --probe compatibility \
  --sdk /private/palm-os-sdk/sdk-5r3
python3 tools/check-compatibility.py \
  --session /private/tx-ready.session \
  --output /private/results/compatibility-02
```

Build the core and synthetic test first. `--core`, `--core-test` and `--probe`
select alternative local binaries. The PRC filename is `FrondCompatProbe.prc`;
its Palm database/launch name is **Frond compatibility**.

Start from a ready session with Palm setup complete and this probe closed. The
harness uses the [developer runner](developer-run.md), including its readiness
checks, private input copies, headless operation, named log export and bounded
cleanup. When `heavy` is installed, each actual firmware run automatically holds
a shared `heavy emulator` slot. Do not wrap the harness in another emulator slot.
`--timeout` is a wall-clock limit per firmware run, including waiting for that
slot (default 300 seconds). Synthetic execution has a 30-second limit.

The harness performs two runs. The first installs a token configuration and the
probe; the second restores that run's saved session, installs a new configuration
and relaunches the existing probe. The second run requires the first token in
both its database record and saved preference before replacing them. This checks
fresh application execution after session restore, rather than merely exporting
the first run's log again. It is not a reset, cold boot or physical flash test.

Both firmware runs check these cases in order:

- `memory`: allocate/lock/unlock/free a movable handle, overlapping `MemMove`,
  resize after unlock and verify preserved bytes.
- `database`: create/open/write/release/close/reopen a record database and check
  exact token bytes. The restored run also verifies the previous token.
- `preferences`: write/read a versioned saved application preference, including
  exact bytes and size; the restored run first verifies the previous token.
- `ticks`: a two-tick `SysTaskDelay` advances `TimGetTicks`; this measures guest
  progress, not physical TX speed.
- `drawing`: create an offscreen window, paint a black pixel, erase it to white,
  read both through RGB APIs, and restore drawing state.
- `native-arm`, `native-thumb`: run the same freestanding calculation in two
  native resources. Check shared big-endian data, native return values, a
  `Call68KFunc` OS `StrLen` trap and a 68k function callback with long/word arguments.
  The Thumb resource enters through an ARM veneer and returns through interworking.
  The builder rejects absolute ARM relocations and requires entry offset zero.
- `event-queue`: enqueue a private user event and observe it in the application's
  real event loop before writing the completion record.

The probe owns `FrondCompatCfg`, `FrondCompatLog`, `FrondCompatData` and the `FrCp`
preference. It replaces those test records **inside the private session copy**.
The log is recreated on each launch. A unique token must match both its beginning
and successful completion; every case must be present once, in order, with the
expected error and value. Old logs, partial exports and a successful host launch
alone cannot pass.

## Evidence and remaining coverage

The output directory must be new and is created with mode 0700. `matrix.json`
records pass/error and individual observations; omitted firmware inputs produce
explicit `skipped` rows. Each firmware subdirectory contains `result.json` with
input paths and SHA-256 hashes (core, session, installed probe/configuration),
socket outcomes, `core.log`, a screenshot, the named `FrondCompatLog` export and
the saved session. A failed invocation leaves diagnostic artifacts and fails the
matrix; inspect its exported records for individual guest errors. Screenshots
are evidence for manual inspection, not an automated visual correctness verdict.
Keep the entire output private: sessions contain firmware, installed apps and
personal data, and even logs/screenshots may contain private content. Generated
SDK binaries stay in ignored `build/`; only original probe source is tracked.

The matrix adds repeatable coverage for basic services and the mixed 68k/native
boundary. It does not run third-party applications or accept their licenses.
SimCity and SunVox remain version/session-specific smoke workloads described in
[application testing](app-testing.md); no fresh third-party verdict follows from
this probe. Native sound stream callbacks and PCM continuity have separate
[audio tests](app-testing.md#pcm-capture-and-continuity). SD, networking and keyboard
acceptance remain separate in [peripherals](peripherals.md). Sleep/wake,
full-screen drawing/UI response, ARMlets using other APIs, native libraries,
exceptions, self-modifying code, Bluetooth and USB are outside this focused
application probe. Record those as untested here, even when other tests exist.
