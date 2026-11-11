#include <cassert>
#include <cstdio>
#include <vector>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_commands.h"
#include "zodiac-graphics-hostdata.h"
#include "zodiac-graphics-keyed-copy.h"

static void checkCommands(bool packedMono) {
    using A = Imx1BootstrapAccess;
    Imx1Eim eim;
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028,4,true,value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c,4,true,value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration state(Imx1GraphicsProfile::virtual5744Compatible8MiB,false,packedMono);
    configureGraphicsInterface(state,eim);
    configureGraphicsController(state,eim);
    struct Op { uint32_t pc, address, value; bool write; };
    for (auto op : {Op{0x200acb98,0x160113e8,0,false},
        {0x200acba8,0x160113e8,0x400,true}, {0x200acbb4,0x16010144,0x2440,true},
        {0x200acbc4,0x160110a8,0x1fff1fff,true}, {0x200acbd0,0x160104dc,0xc0000,true},
        {0x200b8e80,0x16010214,0x10150000,true}, {0x200b8e88,0x16010220,0,true},
        {0x200b8f04,0x16010008,0x04150000,true}, {0x200b8f10,0x16010230,0x80000000,true},
        {0x200b8f20,0x16010210,0x0800000d,true}}) {
        value = op.value;
        assert(state.access(eim,op.pc,op.address,4,op.write,value) == A::ok);
    }
    value = 0;
    assert(state.access(eim,0x200b02ac,0x16011280,4,false,value) == A::ok && value == 0);
    value = 0x10000;
    assert(state.access(eim,0x200b02fc,0x16011280,4,true,value) == A::ok);
    const uint32_t regs[]{0x4a1,0x41b,0x41f,0x472,0x4b2,0x4b1,0x490,0x491,0x46f,0x470,
        0x401,0x402,0x46b,0x46c,0x41e,0x488,0x484,0x471,0x423,0x41d,0x4b3,0x30f,
        0x42e,0x42f,0x430,0x431,0x432,0x433,0x4a0};
    const uint32_t words[]{0,0x55e0b4df,0xffff,0x3f,0xf00a00,0x40030d04,0xffffffff,0,0,0x1e00140,
        0x100000,0x140,0x100000,0x140,0,4,0xf0f0f,0x1fff1fff,0x100000,0,0xffffffff,0x20,
        0,0,0,0,0,0,0};
    std::vector<uint32_t> batch;
    for (unsigned i = 0; i < 28; i += 2) {
        batch.push_back(0x40000000 | regs[i] | (regs[i+1] << 11));
        batch.push_back(words[i]); batch.push_back(words[i+1]);
    }
    for (auto word : {regs[28],words[28],0xc0011a00u,0u,0x014001e0u}) batch.push_back(word);
    Imx1GraphicsMemory memory;
    for (unsigned i = 0; i < batch.size(); ++i) {
        value = batch[i];
        assert(memory.accessGpu(state,eim,0x150000+i*4,4,true,value) == A::ok);
    }
    Imx1GraphicsCommands commands;
    auto submitted = state;
    value = uint32_t(batch.size());
    assert(submitted.access(eim,0x200a9ed4,0x16010220,4,true,value) == A::ok);
    assert(commands.prepare(submitted,eim,memory) == A::ok);
    submitted.startEngine(commands.duration());
    assert(submitted.engineState().nativeOptionWord() == 0x10000);
    value = 0;
    assert(submitted.access(eim,0x200b8cb0,0x16010140,4,false,value) == A::ok && value == 0x80000000);
    assert(memory.accessGpu(submitted,eim,0x100000,2,false,value) == A::ok && value == 0);
    assert(commands.duration() == 153648);
    {
        // The scan status read is let through while the batch draws; other display accesses still wait.
        auto drawing = submitted;
        uint32_t scan = 0xdeadbeef;
        assert(drawing.access(eim,0x200adb08,0x1601054c,4,false,scan) == A::ok && (scan & ~0x400u) == 0);
        assert(drawing.access(eim,0x200adb14,0x1601054c,4,false,scan) == A::ok && (scan & ~0x400u) == 0);
        assert(drawing.access(eim,0x200adb08,0x1601054c,2,false,scan) == A::width);
        assert(drawing.access(eim,0x200adb08,0x1601054c,4,true,scan) == A::sequence);
        // The status query at 200ad7b0 wasn't seen with a batch in the ring.
        scan = 0xdeadbeef;
        assert(drawing.access(eim,0x200ad7b0,0x1601054c,4,false,scan) == A::sequence && scan == 0xdeadbeef);
        assert(drawing.access(eim,0x200aeb68,0x160104d8,4,true,scan) == A::sequence);
        assert(!drawing.engineState().emptyRing());
    }
    assert(submitted.advanceCpuCycles(uint32_t(commands.duration()-1)));
    assert(!submitted.engineState().completed() && submitted.engineState().consumer() == 0);
    assert(commands.complete(submitted,eim,memory) == A::sequence);
    for (unsigned i = 0; i < 8; ++i) {
        assert(submitted.access(eim,0x200b8c64,0x1601021c,4,false,value) == A::ok && value == 0);
        assert(submitted.access(eim,0x200b8c70,0x16010220,4,false,value) == A::ok && value == batch.size());
    }
    assert(submitted.advanceCpuCycles(1) && submitted.engineState().completed());
    assert(commands.complete(submitted,eim,memory) == A::ok && submitted.engineState().emptyRing());
    assert(submitted.engineState().nativeOptionWord() == 0);
    const auto pending = submitted.interruptState().pendingValue();
    assert(submitted.access(eim,0x200b8cb4,0x16010204,4,false,value) == A::ok && value == pending);
    assert(submitted.interruptState().pendingValue() == pending);
    for (uint32_t address : {0x100000u,0x100002u,0x100280u,0x14affeu})
        assert(memory.accessGpu(submitted,eim,address,2,false,value) == A::ok && value == 0xffff);
    assert(memory.accessGpu(submitted,eim,0x14b000,2,false,value) == A::ok && value == 0);
    assert(submitted.access(eim,0x200b8cb0,0x16010140,4,false,value) == A::ok && value == 0);
    value = 0xdeadbeef;
    assert(submitted.access(eim,0,0x16010220,4,true,value) == A::externalInput);
    assert(submitted.access(eim,0x200a9ed4,0x16010220,2,true,value) == A::width);
    assert(submitted.access(eim,0x200a9ed4,0x16010220,4,true,value) == A::sideEffect);
    assert(submitted.engineState().emptyRing());
    auto attempt = [&](std::vector<uint32_t> packet, bool valid) {
        Imx1GraphicsMemory storage;
        for (unsigned i = 0; i < packet.size(); ++i) {
            value = packet[i];
            assert(storage.accessGpu(state,eim,0x150000+i*4,4,true,value) == A::ok);
        }
        auto candidate = state;
        value = uint32_t(packet.size());
        assert(candidate.access(eim,0x200a9ed4,0x16010220,4,true,value) == A::ok);
        Imx1GraphicsCommands work;
        const auto status = work.prepare(candidate,eim,storage);
        assert((status == A::ok) == valid);
        assert(storage.accessGpu(state,eim,0x100000,2,false,value) == A::ok && value == 0);
        assert(state.engineState().emptyRing());
        if (valid) {
            candidate.startEngine(work.duration());
            assert(candidate.advanceCpuCycles(uint32_t(work.duration())));
            assert(work.complete(candidate,eim,storage) == A::ok);
            // Clipped paint at x2,y3 retains adjacent pixels and little-endian colour.
            assert(storage.accessGpu(candidate,eim,0x100784,2,false,value) == A::ok && value == 0x1234);
            assert(storage.accessGpu(candidate,eim,0x100784,1,false,value) == A::ok && value == 0x34);
            assert(storage.accessGpu(candidate,eim,0x100785,1,false,value) == A::ok && value == 0x12);
            for (auto address : {0x100782u,0x100786u,0x100504u,0x100a04u})
                assert(storage.accessGpu(candidate,eim,address,2,false,value) == A::ok && value == 0);
        }
    };
    auto invalid = batch;
    invalid.push_back(0x80000000);
    attempt(invalid,false);
    invalid = batch; invalid.pop_back();
    attempt(invalid,false);
    // Destination range, byte order and scissor fields are validated before any paint.
    invalid = batch; invalid[16] = 0x15fffe;
    attempt(invalid,false);
    invalid = batch; invalid[16] = 0x100001;
    attempt(invalid,false);
    // A later misaligned fill rejects the whole batch before its first valid fill.
    invalid = batch;
    for (auto word : {0x401u,0x100001u,0xc0011a00u,0u,0x00010001u}) invalid.push_back(word);
    attempt(invalid,false);
    invalid = batch; invalid[2] ^= 0x8000;
    attempt(invalid,false);
    invalid = batch; invalid[14] |= 0x80000000;
    attempt(invalid,false);
    invalid = batch; invalid[7] ^= 0x01000000;
    attempt(invalid,false);
    invalid = batch; invalid[8] ^= 1;
    attempt(invalid,false);
    invalid = batch; invalid[5] &= ~1u;
    attempt(invalid,false);
    auto clipped = batch;
    clipped[4] = 0x1234;
    clipped[13] = 0x00030002;
    // 470 holds the first column and row not drawn.
    clipped[14] = 0x00040003;
    clipped[45] = 0x00010002;
    clipped[46] = 0x00020003;
    attempt(clipped,true);
    // Rejected packets never reach memory or publish their candidate control state.
    for (auto invalid : {0xc0011b00u,0x80000000u,0x000007ffu}) {
        value = invalid;
        assert(memory.accessGpu(state,eim,0x150000,4,true,value) == A::ok);
        auto candidate = state;
        value = uint32_t(batch.size());
        assert(candidate.access(eim,0x200a9ed4,0x16010220,4,true,value) == A::ok);
        Imx1GraphicsCommands rejected;
        assert(rejected.prepare(candidate,eim,memory) != A::ok);
        assert(state.engineState().emptyRing());
    }
    // Captured native rectangle submission follows the completed 47-word boot batch.
    const std::vector<uint32_t> rectangleBatch{0x4023841f,0xffff,0x014001e0,
        0x40201401,0x100000,0x1e0,0x4023646b,0x100000,0x1e0,0xc0011a00,0,0x01e00140};
    assert(batch.size() == 47 && submitted.engineState().consumer() == 47);
    auto submitRectangle = [&](std::vector<uint32_t> packet, bool valid) {
        auto candidate = submitted;
        auto work = commands;
        Imx1GraphicsMemory storage;
        for (unsigned i = 0; i < packet.size(); ++i) {
            value = packet[i];
            assert(storage.accessGpu(candidate,eim,0x150000+(47+i)*4,4,true,value) == A::ok);
        }
        for (uint32_t pc : {0x200b01fcu,0x200b0204u}) {
            value = 59;
            assert(candidate.access(eim,pc,0x16010220,4,true,value) == A::externalInput);
            assert(candidate.engineState().emptyRing() && candidate.engineState().producer() == 47);
        }
        value = 59;
        assert(candidate.access(eim,0x200b0200,0x16010220,2,true,value) == A::width);
        value = candidate.engineState().ringBytes()/4;
        assert(candidate.access(eim,0x200b0200,0x16010220,4,true,value) == A::sideEffect);
        value = 59;
        assert(candidate.access(eim,0x200b0200,0x16010220,4,true,value) == A::ok);
        assert(candidate.engineState().consumer() == 47 && candidate.engineState().producer() == 59);
        assert(candidate.access(eim,0x200b0200,0x16010220,4,true,value) == A::sequence);
        assert((work.prepare(candidate,eim,storage) == A::ok) == valid);
        assert(submitted.engineState().emptyRing() && submitted.engineState().producer() == 47);
        for (uint32_t address : {0x100000u,0x1003beu,0x1003c0u,0x14affeu,0x14b000u})
            assert(storage.accessGpu(candidate,eim,address,2,false,value) == A::ok && value == 0);
        if (!valid) return;
        assert(work.duration() == 153613);
        candidate.startEngine(work.duration());
        assert(candidate.advanceCpuCycles(153612));
        assert(!candidate.engineState().completed() && candidate.engineState().consumer() == 47);
        assert(work.complete(candidate,eim,storage) == A::sequence);
        assert(storage.accessGpu(candidate,eim,0x100000,2,false,value) == A::ok && value == 0);
        assert(candidate.advanceCpuCycles(1) && candidate.engineState().completed());
        assert(work.complete(candidate,eim,storage) == A::ok);
        assert(candidate.engineState().emptyRing() && candidate.engineState().consumer() == 59);
        for (uint32_t address : {0x100000u,0x1003beu,0x1003c0u,0x14affeu})
            assert(storage.accessGpu(candidate,eim,address,2,false,value) == A::ok && value == 0xffff);
        assert(storage.accessGpu(candidate,eim,0x14b000,2,false,value) == A::ok && value == 0);
        value = 59;
        assert(candidate.access(eim,0x200b0200,0x16010220,4,true,value) == A::ok);
        assert(candidate.engineState().emptyRing() && candidate.engineState().consumer() == 59);
    };
    submitRectangle(rectangleBatch,true);
    for (unsigned index : {0u,3u,6u,9u}) {
        auto bad = rectangleBatch;
        bad[index] = 0x80000000;
        submitRectangle(bad,false);
    }
    auto badRectangle = rectangleBatch;
    badRectangle[5] = 0x1df;
    submitRectangle(badRectangle,false);
    badRectangle = rectangleBatch;
    badRectangle[11] = 0x01e10140;
    submitRectangle(badRectangle,false);
    checkHostData(submitted,commands,eim);
    checkPackedMono(submitted,commands,eim);
    checkKeyedCopy(submitted,commands,eim);
    submitted.reset(); commands.reset();
    assert(!submitted.engineState().completed() && !submitted.engineState().ringBytes());
    std::puts("PASS: native ring packet decoding, delayed RGB565 solid paint, consumer/busy/readback guards and rejection");
}

int main() {
    checkCommands(false);
    checkCommands(true);
    // The fills ran straight on the bank storage; with every fill through accessGpu the same checks pass.
    const auto direct = Imx1GraphicsCommands::directFillCount;
    assert(direct > 0);
    Imx1GraphicsCommands::directFills = false;
    checkCommands(false);
    checkCommands(true);
    assert(Imx1GraphicsCommands::directFillCount == direct);
    std::printf("PASS: %llu fills completed straight on the bank storage, and the same through accessGpu\n",
                (unsigned long long)direct);
}
