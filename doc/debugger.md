# ARM/Thumb and PACE debugger

Frond's native build includes a small GDB Remote Serial Protocol (RSP) server.
It opens no socket by default. Start the command-line emulator with an explicit
TCP port, for example:

```sh
./out/frond /private/path/tx.session --gdb 3333 --no-sound
gdb-multiarch
```

In GDB:

```gdb
target remote 127.0.0.1:3333
info registers
x/8i $pc
si
break *0xADDRESS
continue
detach
```

Use an ARM-capable GDB with XML target-description support. Frond supplies an
ARM core description; no guest executable or symbol file is required for
address-based debugging. Load matching ARM symbols in GDB yourself if available.
The desktop launcher does not currently expose this option.

`--gdb` accepts ports 1–65535 and binds **only IPv4 loopback, 127.0.0.1**. There
is no remote-bind option, authentication, or encryption. Other local processes
can connect and change guest memory and registers. Do not forward this port to
an untrusted network. Startup waits for the first debugger before initializing
the CPU and running the guest. The first stop is at an ARM/Thumb or optimized PACE instruction
boundary; GDB discovers it with `?`.

Detach acknowledges `D` with `OK`, closes the client, clears all breakpoints, and resumes emulation. EOF, reset, and send errors do the same;
they do not exit Frond. The loopback listener remains available. Reconnecting
stops at the next native or optimized PACE instruction boundary, with fresh debugger state.
Debugger configuration is not stored in guest sessions.

## On Android

Turn on Settings > Debugger > gdb server and set its port (3333 unless you change it).
It applies to the next session you open. The Palm then waits for gdb before it runs,
with a note showing this device's address. From another machine on the same network:

```gdb
target remote PHONE:3333
```

The app passes the core `--gdb` on a loopback port and waits for gdb on the port you set,
on all of the phone's network addresses, with no password. Only turn it on on a network
you trust. For an emulator, forward a port first and connect to it on your computer:

```sh
adb forward tcp:3333 tcp:3333
gdb-multiarch -ex 'target remote 127.0.0.1:3333'
```

The menu's Device section shows whether gdb is connected. Back while the Palm waits for
gdb, or while gdb has it stopped, ends the gdb connection, saves and leaves. gdb gets no
answers while the emulator is hidden (the menu screen is open or you're in another app)
or while the Palm is asleep, since the core only serves gdb between instructions; wake
the Palm first. The Palm runs slower with the gdb server on. Everything below works the
same through the phone.

## Implemented scope

- One emulated ARM CPU and one synthetic RSP thread (ID 1), using all-stop mode.
- Read/write r0–r15 and CPSR, individually (`p`/`P`) or together (`g`/`G`). The
  XML-described bulk packet contains these 17 little-endian 32-bit values;
  CPSR has register number 25. PC reads are instruction addresses with bit 0
  clear. PC writes preserve CPSR.T and align to the current instruction set;
  change CPSR.T explicitly to switch ARM/Thumb. Invalid CPSR modes are rejected.
- Privileged **virtual** memory reads and hexadecimal writes (`m`/`M`) through
  the active MMU/MPU mappings. Accesses use aligned words/halfwords where possible
  and may have MMIO side effects. Unreadable memory returns `E0e`, without
  fabricated zeroes or a successful partial reply. A failed write can have
  changed an initial portion; there is no rollback. All attempted nonempty
  debugger writes discard the instruction cache and its decoded ARM/Thumb
  entries, including writes that fail partway through. Guest MMU/TLB behavior
  otherwise remains in effect.
- Single instruction step and continue (`s`/`c`, optional address), plus
  `vCont;c`/`vCont;s` with optional thread ID 1. Step stops at the next native
  boundary, including after a branch or exception. Ctrl-C interrupts execution;
  while running the socket is polled every 1024 native or optimized PACE instruction boundaries.
- Up to 16 address breakpoints (`Z0`/`Z1`, kind 2 for Thumb or 4 for ARM), checked
  before execution without modifying code bytes. Insertion/removal is
  idempotent. A breakpoint stays active until removed; GDB normally removes it
  to step past it. An ARM breakpoint does not match Thumb at the same address.
- Watchpoint packets (`Z2`/`Z3`/`Z4`, and their removal forms) explicitly
  return the empty unsupported response. See [watchpoints](#watchpoints).
- Fixed 4096-byte incoming packet limit (`PacketSize=1000` in hexadecimal),
  escaped-wire checksums, ACK/NACK, reply retransmission, bounded numeric
  parsing, and exact write lengths. Bad/oversized packets receive NACK without
  executing their commands; malformed supported commands return `E01`.
  Unsupported packets receive an empty reply. Bulk memory replies are limited
  to 2048 bytes; writes must also fit the incoming packet including its header.

The debugger runs on the emulation thread. A stopped CPU or an incomplete packet
at a stop blocks the native frontend and event processing until resumed or
disconnected. There is no non-stop mode or separate debugger UI. Signals,
kill/restart, binary writes, FPA/VFP/WMMX/SPSR registers,
multiple clients and reverse execution are not implemented.

The RSP target architecture and ordinary register packets always describe ARM.
At an optimized PACE stop, `info registers`, `$pc` and `x/i $pc` describe the
**suspended native context**, not the executing 68k program. Use the PACE monitor
commands below for 68k state. Native RSP `s`/`vCont;s`, register writes, memory
writes and continue-with-address return `E16` at a PACE stop. This does **not**
guarantee that GDB's `si`/`stepi` command is rejected: ARM GDB can implement it as
a temporary native breakpoint plus continue, using the suspended ARM PC. Do not
use native `si`, `step`, `next` or `finish` to step PACE. Use the explicit monitor
sequence below. Plain continue, reads, interrupt and detach still work. Native breakpoints match only native execution; they never
match a 68k address. Stepping the native entry instruction can stop at the first
PACE boundary before any 68k instruction executes.

## PACE inspection and breakpoints

```gdb
monitor pace
monitor pace-break 20001000
continue
monitor pace
monitor pace-clear
monitor pace-stop on
continue
monitor pace-last
monitor pace-stop off
detach
```

Replace `20001000` with a **known guest 68k virtual instruction address** for
this run, written in hexadecimal without `0x`. Up to 16 even addresses can be
installed. Duplicate insertion succeeds; `pace-clear` removes all PACE
breakpoints. They stop before execution without patching code bytes. Continuing
at an installed breakpoint stops again: clear it before continuing. They are
address breakpoints, not app-scoped breakpoints; Palm can relocate or reuse code.
68k stepping uses the separate monitor sequence below. Register editing,
relocation and automatic runtime symbol loading are not implemented; see the
byte-verified offline [symbol report](#program-identity-and-private-reports).

`monitor pace` prints one JSON object (`version: 1`) containing D0–D7 (`d`),
A0–A7 (`a`), PC, interpreter SR and the PACE state-block pointer. Numeric values
are unsigned decimal. Reads copy host interpreter state without accessing guest
memory, invoking Palm services or clearing a memory-fault latch. `phase` is
`boundary` between instructions, `exception` after a faulting instruction, or
`dispatch` after a stepped syscall/return/patch trap awaiting its native handling.
Outside optimized PACE execution, the response is `available: false` with
`reason: native-mode`; stale 68k registers are never presented as live registers.
This includes native callouts and firmware paths that have not entered the
optimized interpreter. The state pointer identifies a PACE context, not an app.

`monitor pace-last` returns the latest exceptional interpreter result observed
while this debugger was attached, or `available: false`. It records the attempted
`instruction_pc` separately from the interpreter's resulting `pc`, all 68k
registers, and the numeric `paceStatus`. Memory faults (status 49) also record
`fault_address`, `fault_write` and `fault_status`. Instructions may have partially
changed registers or memory; the snapshot is not a rollback or a claim that all
instructions completed. Opcode bytes are not reread from potentially faulty or
side-effecting memory.

`pace-stop on` stops immediately after such an exception and **before** the
normal native exception/callout handler runs. Continue dispatches the existing
result once, without replaying its memory accesses. This is opt-in; capture alone
does not stop execution. A handled exception is not necessarily an application
crash. Syscalls, returns and TRAP #0/#8 emulator patch paths are excluded. Memory
faults during state transfer or native callouts are outside this instruction
exception capture. There is no persistent crash log without an attached debugger.

PACE breakpoints, exception-stop settings and retained snapshots are cleared on
detach, connection loss, CPU reset and session load. They are not serialized;
existing CPU/PACE session layouts and versions are unchanged. Native breakpoint
lifecycle is unchanged. Debugging still blocks the frontend while stopped.

## One PACE instruction attempt

At an active optimized PACE boundary:

```gdb
monitor pace-step
continue
monitor pace-last-step
monitor pace
```

`pace-step` only arms the **next plain continue** (`c` or `vCont;c`, optionally
thread 1). It does not execute during the monitor request. That continue bypasses
the PACE breakpoint at the current address once, attempts one interpreter
instruction, then reports an ordinary `S05` stop. The breakpoint remains installed.
Repeated steps use the same two-command sequence; `monitor pace-step off` cancels
an armed step. No guest code is patched and the ARM target description is unchanged.

`pace-last-step` returns a version 1 snapshot with `instruction_pc` (the attempted
instruction), resulting `pc`, D/A/SR registers, numeric interpreter `status`, and
fault details when applicable. A normal instruction has status 0 and phase
`boundary`. Variable-length instructions, branches (including self branches),
calls and returns each count as one attempt, regardless of their resulting PC.
Arming a new step replaces the last record with `available: false, reason: armed`.
Reset, restore and connection changes clear both the record and any pending step.

The stop occurs **after the interpreter returns and before pending syscall,
return, exception or patch-trap dispatch**. A memory-faulting instruction may have
partially changed registers or completed earlier accesses; the snapshot preserves
those effects. An instruction-fetch fault is still one attempted step. This is
not rollback, an instruction retry, or a promise that the instruction retired.
The normal dispatch runs exactly once on the next continue. `pace-step` returns
`E16` while that dispatch is pending, including at `pace-stop on` exception stops;
it cannot skip an unresolved exception or silently run native handlers to find
another 68k instruction. Use continue with a known PACE/native breakpoint, or
Ctrl-C, to reach a later boundary. `pace-stop on` and stepping together produce
one exception stop, not two.

If a guest IRQ transfers execution to native code before the next armed attempt,
the step is cancelled at the native boundary **before the first native instruction
executes**. `pace-last-step` reports `available: false` and
`reason: interrupted-before-instruction`. Ctrl-C observed before the attempt has
the same result with `S02`; already completed attempts remain completed. Guest
interrupt delivery and peripherals otherwise keep the emulator's existing batch
and scheduler timing. Stepping does not mask interrupts or add native execution
in pursuit of a later PACE stop. Direct CPU reset also exits PACE mode before
fetching the native reset vector.

A stepped store followed by another store was tested with **GDB 16.3 multiarch**:
the first stop has 68k PC `0x31002` and one successful store; the second explicit
step has PC `0x31004` and two stores. A real-client partial-write test also checks
that native abort dispatch does not replay the completed halfword. These stops
use ordinary traps, not RSP hardware-watchpoint replies, so they do not request
ARM GDB's watchpoint completion step.

## Program identity and private reports

`monitor attribution` reports foreground-app metadata at the qualified TX native
UI wait. It follows the UI task's independent TLS/root-app/open-database chain,
checks the firmware route and database header, and rejects nested launches,
transitions and inconsistent or unmapped metadata. It reads RAM/ROM without guest
API calls, guest writes, page-table MMIO reads or TLB changes. At other stops,
including PACE, `foreground_app.available` is false. This is deliberately limited
to the qualified normal-launch UI root; it is not a general Palm OS app lookup.
Names and type/creator bytes are hex encoded to preserve their original bytes.

The current PC can belong to firmware, a library or a native callout while an app
is in the foreground. `code_owner` remains unavailable, and PACE snapshots retain
`program_identity: null`. A state pointer or 68k PC alone cannot establish which
Palm database owns the code. For a reproducible report, copy
one JSON object printed by `monitor pace` or `monitor pace-last` into a private
file, then attach the identity of the PRC/PDB you intended to run:

```sh
python3 tools/pace-report.py --context /private/pace-context.json \
  --program /private/MyApp.prc > /private/pace-report.json
```

The helper reads the local Palm database header and directory and hashes the
file. It includes the database name, raw type/creator bytes, version and resource
IDs/file offsets. `declared_program.active_app_verified` is always false and
`guest_load_address` is null. File offsets are never treated as guest addresses;
the supplied file may differ from the installed or active application. The
helper does not connect to the emulator. Stack unwinding and fatal-alert
detection are not provided. Treat reports as private: even
registers and database names can contain app/user data. Do not commit reports,
GDB transcripts, firmware or session copies.

For a matching 68k compiler executable, add `--executable /private/MyApp`.
The helper extracts `.text` with `m68k-palmos-objdump`/`objcopy` (override the
tool prefix with `--tool-prefix`) and requires an exact, unique match
inside one `code` PRC resource. Text symbols from `nm` are then expressed
as resource offsets. A mismatched or ambiguous executable fails; neither linker
addresses nor PRC file offsets are used as guest load addresses.

An optional `--runtime-map /private/runtime-map.json` binds those offsets to a
captured guest interval. A version 1 map contains `program_sha256`, `context_pc`,
the complete `context` monitor object, and `resources` entries with `type_hex`,
`id`, `address`, `size`, and the SHA-256 of the complete live resource bytes.
The helper requires the exact context, matching program/resource hashes and sizes,
valid nonoverlapping address ranges, and a PC within a mapped resource. It reports
the nearest preceding text symbol and displacement, without asserting function
boundaries. The map is supplied capture evidence: the helper cannot authenticate
its origin or establish that it describes the current emulator stop. Capture it
at the same stop as the context. Byte equivalence establishes a code location;
it does not establish which database or foreground application owns the bytes.
`active_app_verified` remains false for this mapping. The SDK debugger acceptance
probe produces such a map only after comparing the complete locked live resource
against a unique local PRC resource.
Relocated or modified live bytes that differ from the supplied resource fail the
hash check; the helper does not guess relocations or recover stripped symbols.

For app installation and replay, use the [developer workflow](developer-run.md)
to produce a private working session, then open that session with `--gdb`.
The runner does not automate an interactive debugger session. Avoid concurrent
DesktopControl commands while stopped: the emulation thread cannot serve them.

## Validation

```sh
heavy build make -j2 test-gdbstub test-pace-debug test-pace-step test-debugger-inspect
heavy build make -j2 test-compatibility test-tx-state test-instruction-cache test-arm-conditions test-cpu
```

`tests/gdbstub-target.cpp` supplies a real ARM CPU, synthetic mapped RAM, an
unmapped section, and counted data accesses; it contains no firmware.
`tests/gdbstub.py` speaks RSP over TCP and checks register/PC/CPSR edits, ARM and
Thumb stepping and breakpoints, code replacement after cached execution, memory
faults and partial writes, checksums, packet limits, fragmentation, escaped
input, retransmission, Ctrl-C, detach, EOF/reset, reconnect and the kernel's
loopback bind address. The suite validates explicit rejection of read/write/access
watchpoints and checks that a subsequent synthetic store still completes normally.
Each test terminates only its own synthetic target.
If `gdb-multiarch` is available, an additional batch-client test verifies XML
negotiation, register writes, ARM/Thumb stepping, address breakpoints and detach. Set `FROND_GDB` to
an alternate ARM-capable GDB executable; this optional test is skipped otherwise.
Use the local heavy build/emulator wrappers when sharing a development machine.

`tests/pace-debug.py` uses synthetic RAM and the real ARM-to-PACE entry/state
ABI, checking known D/A registers, SR and PC, pre-instruction breakpoints, native
command guards, Ctrl-C in a PACE loop, reconnect, reset/restore cleanup, exception
dispatch, instruction fetch faults, a partially completed write, reply
retransmission and GDB monitor interoperability.
The partial write test counts successful accesses to detect replay. The
compatibility core also asserts snapshot inspection performs no memory access
and preserves the fault latch. Report tests use a generated Palm database header.
These mandatory tests need no ROM, SDK, application or private fixture.
`tests/debugger-inspect.cpp` also checks device-backed page-table rejection,
unchanged TLB state, read-only RAM classification, bounded buffer failures and
unchanged RAM/dirty pages. Attribution protocol tests reject unqualified native
and PACE contexts and retain no identity across reset/load.

`tests/pace-step.py` additionally checks one-attempt semantics for multiword
instructions, branches, BSR/RTS, PACE exit, syscalls and patch traps; instruction
and extension-fetch faults; partial longword/MOVEM stores; IRQ and Ctrl-C
interruption; cancellation; reply retransmission; detach/EOF; and reset/restore
with an in-flight step. The GDB client tests run when `FROND_GDB` names a working
ARM-capable GDB (or `gdb-multiarch` is on PATH); a skip is not client validation.
The mandatory tests use only synthetic RAM. The optional SDK probe below also
validates known instruction semantics against a qualified TX session with real GDB.

Optional real TX smoke test, using your own already booted session:

```sh
python3 tests/pace-debug-tx.py --session /private/tx-ready.session \
  --output /private/results/pace-smoke-01 --timeout 90
```

It takes a `heavy emulator` slot when available, uses a private input copy with
SDL dummy drivers, interrupts until an optimized PACE boundary is observed,
checks repeatable register snapshots, detaches, and terminates only its own
process group. The deadline includes slot/startup time. It writes `core.log`,
`result.json` and the input copy outside the checkout. Failure to observe PACE
before the deadline fails the test; it does not claim app execution passed.
This smoke test does not validate program attribution or known-address
breakpoints on real TX applications.

For known instruction semantics, build the SDK probe and supply it to the same
driver with an ARM-capable GDB:

```sh
python3 tools/build-palm-probe.py --probe debugger --sdk /private/sdk-5r3
FROND_GDB=/path/to/gdb-multiarch python3 tests/pace-debug-tx.py \
  --session /private/tx-ready.session --probe build/debugger-probe/FrondDebugProbe.prc \
  --output /private/results/debugger-acceptance-01 --timeout 180
```

The driver uses normal installation and launch on copied state, with no input
events. The app locks its code resource and supplies a descriptor; GDB compares
the full live resource with a unique PRC resource before using its entry points.
The acceptance checks repeatable snapshots, known-address breakpoints, arithmetic,
stores, a taken branch, and explicit PACE step/continue. An overlapping `MemMove`
has distinct outcomes for zero, one and repeated dispatches. A timed deliberate
divide-by-zero checks one-attempt exception capture, rejection of another PACE
step, dispatch to the qualified native callout, and its first native instruction.
It does not establish firmware recovery from the exception or complete fatal
handler execution. This path has passed with a private TX session and GDB 16.3;
other firmware still requires qualification. Results include context and runtime
resource maps for private symbol reports, plus hashes and input-preservation checks.

## Watchpoints

ARM/Thumb and PACE watchpoints are not supported. A post-access stop is not
compatible with the way ARM GDB resumes a watched instruction; a safe implementation
must also avoid repeating MMIO or partial multi-access operations. The
`tests/watchpoint-contract.py` diagnostic checks this limitation with a real GDB
client when one is available.

The pre-access investigation established two bounded designs in an isolated,
firmware-free prototype with real GDB 16.3. Native ARM/Thumb transfer planning
can stop before any transfer or register writeback, discard the fetched plan,
and let GDB complete the instruction once. A later-word STM/PUSH watch must stop
before the first word; condition checks, actual aligned bus ranges and transfer
order must match the executor. A production implementation needs shared decoding
and coverage of register offsets, swaps, double transfers, coprocessors and helper
paths. Speculative MMIO or page-table reads cannot be used to predict accesses.

PACE can instead suspend on its live host call stack before a logical memory
access. A custom monitor stop with ordinary `S05` can continue that exact access
without repeating earlier MMIO reads or partial writes. Such a stop exposes
partial instruction state and must have a distinct continuation phase; ARM GDB's
hardware-watchpoint completion cannot step a suspended 68k instruction correctly.
Reset/load, debugger mutation, disconnect, instruction-fetch versus data-read
classification and optimized helper coverage require a complete lifecycle contract.
These are design results, not supported commands. The production stub continues
to reject `Z2`/`Z3`/`Z4` explicitly until that coverage is implemented and tested.

Protocol references: [GDB RSP overview](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Overview.html),
[packets](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Packets.html), and
[target descriptions](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Standard-Target-Features.html).
