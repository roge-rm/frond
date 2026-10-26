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
  return the empty unsupported response. See the investigation below.
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
commands below for 68k state. Native `s`/`si`, register writes, memory writes and
continue-with-address return `E16` at a PACE stop. Plain continue, reads, interrupt
and detach still work. Native breakpoints match only native execution; they never
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
No 68k single-step, register editing, relocation or symbol mapping is implemented.

`monitor pace` prints one JSON object (`version: 1`) containing D0–D7 (`d`),
A0–A7 (`a`), PC, interpreter SR and the PACE state-block pointer. Numeric values
are unsigned decimal. Reads copy host interpreter state without accessing guest
memory, invoking Palm services or clearing a memory-fault latch. `phase` is
`boundary` before an instruction or `exception` after a faulting instruction.
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

## Program identity and private reports

The debugger reports `program_identity: null`: no validated guest database/app
identity lookup is available. A state pointer or 68k PC alone cannot establish
which Palm application owns the current code. For a reproducible report, copy
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
helper does not connect to the emulator. No symbols, stack unwinding, active-app
attribution or fatal-alert detection are claimed. Treat reports as private: even
registers and database names can contain app/user data. Do not commit reports,
GDB transcripts, firmware or session copies.

For app installation and replay, use the [developer workflow](developer-run.md)
to produce a private working session, then open that session with `--gdb`.
The runner does not automate an interactive debugger session. Avoid concurrent
DesktopControl commands while stopped: the emulation thread cannot serve them.

## Validation

```sh
heavy build make -j2 test-gdbstub test-pace-debug
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
breakpoints on real TX applications. No private TX fixture was used to validate
this change.

## Watchpoint investigation and blocker

A prototype moved watchpoint detection after each successful CPU memory access
and queued a stop for the next native instruction boundary. Raw TCP tests of
ARM/Thumb loads, stores and multiple stores confirmed each access executed once,
with the complete instruction's memory effects, next PC and base-register
writeback visible. However, a **GDB 16.3 multiarch client** then automatically
stepped another instruction: a store at `0x12000` reported by the prototype at
`0x12004` appeared in GDB at `0x12008`, after the following load also executed.

ARM GDB sets `have_nonsteppable_watchpoint`, expecting a synchronous watchpoint
trap **before** its triggering access. It removes the watchpoint and steps the
instruction to finish it. This behavior and the pre-access ARM convention are
explained in the [GDB ARM watchpoint discussion](https://sourceware.org/legacy-ml/gdb-patches/2014-09/msg00846.html).
The RSP subset implemented here has no negotiation for post-instruction watch
semantics. Swallowing GDB's next step would also swallow a real user's single
step; reporting the old PC would misrepresent completed instruction state.
Consequently the prototype is not shipped. Watchpoints need a design compatible
with ARM GDB's pre-access convention, with no replay of MMIO or partial
multi-access instructions, plus real-client and counted ARM/Thumb tests.

Protocol references: [GDB RSP overview](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Overview.html),
[packets](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Packets.html), and
[target descriptions](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Standard-Target-Features.html).
