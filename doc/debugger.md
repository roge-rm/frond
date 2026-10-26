# Native ARM/Thumb debugger

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
the CPU and running the guest. The first stop is at an ARM/Thumb instruction
boundary; GDB discovers it with `?`.

Detach acknowledges `D` with `OK`, closes the client, clears all breakpoints, and resumes emulation. EOF, reset, and send errors do the same;
they do not exit Frond. The loopback listener remains available. Reconnecting
stops at the next native instruction boundary, with fresh debugger state.
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
  while running the socket is polled every 1024 native instruction boundaries.
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
kill/restart, binary writes, monitor commands, FPA/VFP/WMMX/SPSR registers,
multiple clients and reverse execution are not implemented.

This is **native ARM/Thumb debugging**, not Palm app-aware debugging. It does
not expose 68k registers, PACE instruction stepping, Palm database/app identity,
relocation, A-traps, app-scoped breakpoints or Palm-aware symbols. The optimized
PACE loop has no native PC hook; native step, interrupt and reconnect
notifications can be delayed until execution returns from it. No
firmware or real Palm application debugging has been validated by these tests.

## Validation

```sh
make test-gdbstub
make test-instruction-cache test-arm-conditions test-cpu
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
