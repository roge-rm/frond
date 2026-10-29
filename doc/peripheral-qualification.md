# Repeatable peripheral qualification

`tools/qualify-peripherals.py` runs staged JSON plans against private copies of
owned Palm sessions. Run from a source checkout with `heavy` on PATH:

```sh
python3 tools/qualify-peripherals.py --plan owned-plan.json \
  --output build/qualification/new-run --timeout 900
```

Use a new output directory for every run. The runner creates it with mode 0700,
copies sessions, starts each core through `heavy emulator`, waits for the desktop
handshake and executes steps in order. It hashes original cores, sessions, the
plan and declared additional inputs before and after execution. Keep firmware,
applications, sessions, captures and private plans in ignored local storage.

Plans contain `nodes`, optional additional `inputs`, and ordered `steps`. Paths
are relative to the working directory. `$OUT` expands to the new result directory
in arguments and replay text. Process arguments use arrays without a shell.

```json
{
  "nodes": {
    "sender": {"core": "out/frond", "session": "owned-sender.frond"},
    "receiver": {"core": "out/frond", "session": "owned-receiver.frond"}
  },
  "inputs": ["tools/check-irda-probe.py"],
  "steps": [
    {"id": "sender-ready", "node": "sender", "command": "guest-ready",
     "retry_ready": true, "timeout": 20},
    {"id": "receiver-ready", "node": "receiver", "command": "guest-ready",
     "retry_ready": true, "timeout": 20},
    {"id": "payload", "check": ["python3", "tools/check-irda-probe.py",
     "$OUT/sender.zip", "$OUT/receiver.zip",
     "--sender-before", "$OUT/sender-before.zip",
     "--receiver-before", "$OUT/receiver-before.zip"]}
  ]
}
```

This fragment shows the readiness barrier and evidence check. Insert device
setup, transfer and export steps before `payload`; missing exports fail the
checker. Use awake sessions and the normal Palm UI to enable receiving services.
The runner does not bypass drivers or force unsupported native API boundaries.

Each step needs a unique simple `id` and exactly one action:

| Action | Fields and behaviour |
| --- | --- |
| `command` | `node`, command name, optional `argument`; require success. `retry_ready: true` retries only `not-ready` within the deadline. Optional `expect_code` and `expect_message` require exact replies. |
| `replay` | `node`, inline replay text; wait until the core reports completion. Normal input can dismiss a dialog or return to the launcher before checking readiness. |
| `service` | Argument array for an owned relay or protocol peer; combined output goes to `ID.log`. Follow with a readiness marker check. Optional `allow_exit: [0]` permits a peer that normally exits successfully after completing its work. |
| `wait_log` | Log path plus a nonempty `contains` marker; wait within the deadline while checking owned process health. Use a new log under `$OUT`. |
| `wait` | Positive settling interval in seconds, within the step timeout. |
| `stop` | ID of a service started by this run; stop its process group. |
| `check` | Argument array for an independent checker. Require exit zero and a JSON object with `status: "pass"`. Retain stdout and stderr separately. |

Each step has a wall timeout (default 30 seconds). The overall run has a separate
budget. Nodes may set `startup_timeout` (default 60 seconds), `command_timeout`
(default 10 seconds) and extra CLI `args`. Increase both the step and command
budgets for measured operations such as E2's fallback database exports.
Use `expect_message` when success
also requires a particular endpoint or power state: a successful RPC alone
does not prove a transition.

For a named database export, use a command such as:

```json
{"id":"sender-before","node":"sender","command":"export-db",
 "argument":"$OUT/sender-before.zip\nFrondIRLog","retry_ready":true,"timeout":10}
```

Export the original log before each attempt and pass it to the checker so an
earlier success cannot qualify a new attempt. The Bluetooth checker also accepts
an independent peer's received binary payload.

`result.json` records the failing stage, command replies, checker verdicts,
elapsed time, input guards and artifact hashes. An error blocks downstream work.
The runner stops only process groups it created, including queued `heavy`
wrappers. Guests exit through `quit` on success; use an explicit `save` step to
retain their final state. On failure the runner attempts a separate saved session
for each live guest, with a five-second cleanup budget per guest and no UI retry.
The report records whether each failure save succeeded. Logs and screenshots
remain even when the guest cannot be saved.

Run the firmware-free orchestration tests with `make test-peripheral-qualification`.
A real E2 original-stack Bluetooth send has also passed through this runner on
the production core, including exact payload and unchanged log-history checks.
Original-stack cancellation/retry, raw data and lifecycle checks also use this
workflow. Each device still needs its own firmware evidence; a passing runner
test does not qualify a new ROM or peripheral.

Future devices reuse controller models, peers, checkers and orchestration.
Original ROM behaviour, board wiring, power transitions and firmware acceptance
still require device-specific evidence.
