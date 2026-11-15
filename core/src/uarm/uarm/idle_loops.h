#ifndef _IDLE_LOOPS_H_
#define _IDLE_LOOPS_H_

#include <cstdint>

// The PalmOS 5 kernel wait loops the Fast engine skips (cpuPrvIdleSkip in CPU.cpp). Each pass
// reads OSCR, sets bit 0 of a kernel byte, calls the IRQ window (ICCR = 1, IRQs unmasked for
// one instruction), clears the bit, reads OSCR again, adds the difference to an idle counter
// and loops while the kernel's wake word is 0. Only an IRQ taken in the window ends it.
// Matched by these words at the loop head and at the window, wherever they sit.

enum class IdleLoopKind : uint8_t { zire72, tx };

struct IdleLoop {
    IdleLoopKind kind;
    const uint32_t *words;  // from the head to the branch back to it
    uint32_t count;
    int32_t window;          // the IRQ window, from the head
    uint32_t windowReturn;   // the window call's return address, from the head
    uint32_t pass;           // instructions in a pass that goes straight to the body
};

// The IRQ window, the same in both ROMs. Its literal (the ICCR address) is at +0xe8.
constexpr uint32_t kIdleWindowWords[] = {
    0xe14f2000, 0xe92d4004, 0xe59f00d8, 0xe3a01001, 0xe5801000, 0xe10f0000, 0xe3c01080, 0xe121f001,
    0xe24ff004, 0xe121f000, 0xe8bd4004, 0xe161f002, 0xe12fff1e,
};
constexpr uint32_t kIdleWindowCount = sizeof(kIdleWindowWords) / 4;
constexpr uint32_t kIdleWindowLiteral = 0xe8;

// Zire 72 at 20092310, window at 20091da0. r4 the OS timer base, r5 points to the kernel
// globals, r6 -1, r7 the 50 ms threshold, r8 the idle counter, r10 set once past it.
constexpr uint32_t kIdleLoopZire72[] = {
    0xe1570000, 0x9a000001, 0xe1560000, 0x1a000008, 0xe35a0000, 0x1a000006, 0xe3a00012, 0xeb000124,
    0xe5980000, 0xe1560000, 0x03a00000, 0x05880000, 0xe3a0a001, 0xe594b010, 0xe5950000, 0xe2801a02,
    0xe5d101d0, 0xe3800001, 0xe5c101d0, 0xebfffe8f, 0xe5950000, 0xe2801a02, 0xe5d101d0, 0xe20000fe,
    0xe5c101d0, 0xe5941010, 0xe5980000, 0xe5952000, 0xe040000b, 0xe0800001, 0xe5880000, 0xe2821a02,
    0xe59111c0, 0xe3510000, 0x0affffdc,
};

// T|X at 200a5a94 (RAM copy of NAND), window at 200ac154. r5 the idle counter, r7 the OS
// timer base, r8 + 20 points to the kernel globals, r4 set once past the threshold.
constexpr uint32_t kIdleLoopTx[] = {
    0xe5950000, 0xe250cb9e, 0x225ccfb1, 0x23540000, 0x1a000005, 0xe3a0000c, 0xeb0016a5, 0xe5950000,
    0xe3a04001, 0xe3700001, 0x0585a000, 0xe5976010, 0xe5980014, 0xe2800d80, 0xe2800f74, 0xe5d01000,
    0xe3811001, 0xe5c01000, 0xeb00199c, 0xe5980014, 0xe2800d80, 0xe2800f74, 0xe5d01000, 0xe3c11001,
    0xe5c01000, 0xe5970010, 0xe5951000, 0xe0400006, 0xe0800001, 0xe5850000, 0xe5980014, 0xe2800d80,
    0xe59001c0, 0xe3500000, 0x0affffdc,
};

constexpr IdleLoop kIdleLoops[] = {
    {IdleLoopKind::zire72, kIdleLoopZire72, sizeof(kIdleLoopZire72) / 4, -0x570, 0x50, 39},
    {IdleLoopKind::tx, kIdleLoopTx, sizeof(kIdleLoopTx) / 4, 0x66c0, 0x4c, 42},
};

// The kernel globals' wake word and window byte, from the globals pointer.
constexpr uint32_t kIdleWakeWord = 0x21c0, kIdleWindowByte = 0x21d0;

#endif
