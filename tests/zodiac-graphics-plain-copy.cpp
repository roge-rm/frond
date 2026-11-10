#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_commands.h"

using A = Imx1BootstrapAccess;
namespace {
struct Fixture {
    Imx1Eim eim;
    Imx1Cs5Configuration state;
    Imx1GraphicsCommands commands;
    Imx1GraphicsMemory memory;
    explicit Fixture(bool enabled = true)
        : state(Imx1GraphicsProfile::virtual5744Compatible8MiB, enabled) {
        uint32_t value = 0x3e00;
        assert(eim.access(0x00220028,4,true,value) == Imx1EimAccess::ok);
        value = 0x11111e01;
        assert(eim.access(0x0022002c,4,true,value) == Imx1EimAccess::ok);
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
        const uint32_t regs[]{0x4a1,0x41b,0x41f,0x472,0x4b2,0x4b1,0x490,0x491,0x46f,0x470,
            0x401,0x402,0x46b,0x46c,0x41e,0x488,0x484,0x471,0x423,0x41d,0x4b3,0x30f,
            0x42e,0x42f,0x430,0x431,0x432,0x433,0x4a0,0x48f};
        const uint32_t words[]{0,0x55e0b4df,0xffff,0x3f,0xf00a00,0x40030d04,0xffffffff,0,0,0x1e00140,
            0x100000,0x140,0x100000,0x140,0,4,0xf0f0f,0x1fff1fff,0x100000,0,0xffffffff,0x20,
            0,0,0,0,0,0,0,0xffffffff};
        std::vector<uint32_t> init;
        for (unsigned i = 0; i < 30; ++i) { init.push_back(regs[i]); init.push_back(words[i]); }
        assert(append(init,0x200a9ed4) == A::ok);
        finish();
    }
    void write(uint32_t address, uint32_t value, uint32_t size = 2) {
        assert(memory.accessGpu(state,eim,address,size,true,value) == A::ok);
    }
    uint32_t read(uint32_t address, uint32_t size = 2) {
        uint32_t value = 0;
        assert(memory.accessGpu(state,eim,address,size,false,value) == A::ok);
        return value;
    }
    A append(const std::vector<uint32_t>& batch, uint32_t pc = 0x200a9ed4) {
        const auto begin = state.engineState().producer();
        for (unsigned i = 0; i < batch.size(); ++i) write(0x150000+((begin+i)&16383)*4,batch[i],4);
        auto candidate = state;
        uint32_t end = (begin+uint32_t(batch.size()))&16383;
        auto status = candidate.access(eim,pc,0x16010220,4,true,end);
        if (status == A::ok) status = commands.prepare(candidate,eim,memory);
        if (status == A::ok) {
            candidate.startEngine(commands.duration());
            state = candidate;
        }
        return status;
    }
    void finish() {
        assert(state.advanceCpuCycles(uint32_t(commands.duration())));
        assert(commands.complete(state,eim,memory) == A::ok);
    }
};
constexpr uint32_t source = 0xe03000, destination = 0xe01000;
std::vector<uint32_t> packet() {
    return {0x41b,0x5598b4df,0x4b2,0x00cc0a00,0x4b1,0x40030d04,0x488,4,
        0x401,destination,0x402,13,0x46b,source,0x46c,9,0x46f,0,0x470,0x0014000d,
        0xc0021b00,0x00020001,0x00050003,0x00030002};
}
void change(std::vector<uint32_t>& p, uint32_t reg, uint32_t value) {
    p.insert(p.begin()+20,{reg,value});
}
// Source rows and expected destination positions are explicit, with unequal strides and origins.
void planes(Fixture& f) {
    for (unsigned i = 0; i < 13*8; ++i) f.write(destination+i*2,0xa55a);
    for (unsigned i = 0; i < 9*5; ++i) f.write(source+i*2,0xbeef);
    for (auto sample : {std::pair<unsigned,uint32_t>{11,0}, {12,0xf800}, {13,0x1234},
                        {20,0x5678}, {21,0xffff}, {22,0xabcd}})
        f.write(source+sample.first*2,sample.second);
}
void check(Fixture& f, bool clipped = false) {
    for (unsigned i = 0; i < 13*8; ++i) {
        uint32_t expected = 0xa55a;
        if (!clipped) {
            if (i == 44) expected = 0;
            if (i == 45) expected = 0xf800;
            if (i == 46) expected = 0x1234;
            if (i == 57) expected = 0x5678;
        }
        if (i == 58) expected = 0xffff;
        if (i == 59) expected = 0xabcd;
        assert(f.read(destination+i*2) == expected);
    }
    assert(f.read(source+22) == 0 && f.read(source+26) == 0x1234);
    assert(f.read(destination+118,1) == 0xcd && f.read(destination+119,1) == 0xab);
}
void geometry() {
    Fixture f; planes(f);
    auto p = packet();
    const auto consumer = f.state.engineState().consumer();
    const auto pending = f.state.interruptState().pendingValue();
    assert(f.append(p) == A::ok && f.commands.duration() == p.size()+7);
    uint32_t busy = 0;
    assert(f.state.access(f.eim,0x200b8cb0,0x16010140,4,false,busy) == A::ok && busy == 0x80000000);
    assert(f.state.advanceCpuCycles(uint32_t(f.commands.duration()-1)));
    assert(f.commands.complete(f.state,f.eim,f.memory) == A::sequence);
    assert(f.state.engineState().consumer() == consumer && f.read(destination+88) == 0xa55a);
    assert(f.state.advanceCpuCycles(1) && f.commands.complete(f.state,f.eim,f.memory) == A::ok);
    check(f);
    assert(f.state.engineState().consumer() == consumer+p.size() && f.state.engineState().emptyRing());
    assert(f.state.access(f.eim,0x200b8cb0,0x16010140,4,false,busy) == A::ok && busy == 0);
    assert(f.state.interruptState().pendingValue() == pending);
    planes(f); p = packet(); p.erase(p.begin()+2,p.begin()+4);
    p.insert(p.begin(),{0x4b2,0x00f00a00});
    assert(f.append(p) == A::ok); f.finish(); check(f);
    planes(f); p = packet(); change(p,0x46f,0x00040006);
    assert(f.append(p) == A::ok && f.commands.duration() == p.size()+3);
    f.finish(); check(f,true);
    planes(f); p = packet(); change(p,0x46f,0x0008000a);
    assert(f.append(p) == A::ok && f.commands.duration() == p.size()+1);
    f.finish();
    for (unsigned i = 0; i < 13*8; ++i) assert(f.read(destination+i*2) == 0xa55a);
    // The text producer's foreground and background in 490 and 491 don't change the result.
    planes(f); p = packet(); change(p,0x46f,0); change(p,0x490,0); change(p,0x491,0xffff);
    assert(f.append(p) == A::ok); f.finish(); check(f);
    // Any other pair in 490 and 491 is refused.
    for (auto colours : {std::pair<uint32_t,uint32_t>{0,0}, {0xffffffff,0xffff}, {0x1234,0xffff}}) {
        planes(f); p = packet(); change(p,0x46f,0); change(p,0x490,colours.first); change(p,0x491,colours.second);
        assert(f.append(p) == A::sideEffect);
    }
}
void guards() {
    Fixture f; planes(f);
    // Keep accepted work pending while failed candidates try to change every relevant state group.
    const std::vector<uint32_t> solid{0x401,destination,0x402,13,0xc0011a00,0,0x00010001};
    assert(f.append(solid,0x200b0200) == A::ok);
    const auto duration = f.commands.duration();
    const auto clock = f.state.clockState().count();
    const auto producer = f.state.engineState().producer(), consumer = f.state.engineState().consumer();
    const auto pending = f.state.interruptState().pendingValue();
    auto reject = [&](const std::vector<uint32_t>& bad, uint32_t pc = 0x200a9ed4) {
        const auto dirty = f.memory.dirty(1).generation;
        const auto status = f.append(bad,pc);
        if (status == A::ok) {
            std::fprintf(stderr,"Accepted invalid plain batch at %08x:",pc);
            for (auto word : bad) std::fprintf(stderr," %08x",word);
            std::fprintf(stderr,"\n");
        }
        assert(status != A::ok);
        assert(f.commands.duration() == duration && f.state.engineState().producer() == producer);
        assert(f.state.engineState().consumer() == consumer && f.state.clockState().count() == clock);
        assert(f.state.interruptState().pendingValue() == pending && f.memory.dirty(1).generation == dirty);
        assert(f.read(destination+88) == 0xa55a && f.read(destination) == 0xa55a);
    };
    for (auto item : {std::pair<uint32_t,uint32_t>{0x41b,0x4598b4df},{0x41b,0x5598b4de},
        {0x4b2,0x01cc0a00},{0x4b2,0x00cc0b00},{0x4b1,0x40057d04},{0x488,0x80004},
        {0x48f,0xffff},{0x4b3,0xffff},{0x41d,1},{0x41e,1},
        {0x30f,0},{0x472,0x3e},{0x471,0x1ffe1fff},{0x4a0,1},{0x4a1,1},
        {0x42e,1},{0x42f,1},{0x430,1},{0x431,1},{0x432,1},{0x433,1},
        {0x401,0xe01001},{0x401,0xfffffffe},{0x401,0x15fffc},{0x402,0},{0x402,8193},{0x402,7},
        {0x46b,0xe03001},{0x46b,0xfffffffe},{0x46b,destination+64},{0x46c,0},{0x46c,8193},{0x46c,4},
        {0x46f,0x80000000}}) {
        auto bad = packet(); change(bad,item.first,item.second); reject(bad);
    }
    for (auto item : {std::pair<unsigned,uint32_t>{20,0xc0031b00},{21,0xffff0000},
        {21,0x00090001},{22,0x000d0000},{22,0x00002000},{23,0},{23,0x00010000},{23,0xffff0001}}) {
        auto bad = packet(); bad[item.first] = item.second; reject(bad);
    }
    for (size_t size = 1; size < packet().size(); ++size) {
        const auto p = packet();
        // Incomplete state-only candidates are rejected once plain GMC has been selected.
        reject({p.begin(),p.begin()+size});
    }
    auto bad = packet(); bad.push_back(0); reject(bad);
    bad = packet(); bad.insert(bad.begin(),{0xc0011a00,0,0x00010001}); reject(bad);
    for (uint32_t pc : {0x200aa504u,0x200b0200u,0x200ac300u,0x200a9a9cu,0x200a9ed8u}) reject(packet(),pc);
    // Cross-route state-only/keyed substitutions cannot complete a plain batch.
    bad = packet(); bad.resize(20); reject(bad);
    bad = packet(); bad[20] = 0xc0051c00; reject(bad);
    bad = packet(); bad.insert(bad.begin(),{0x4b2,0x01cc1300});
    // GMC changes only its qualified source1/ROP/operation bits; source2 survives.
    bad.erase(bad.begin()+4,bad.begin()+6); reject(bad);
    assert(f.append(packet()) == A::ok);
    assert(f.state.advanceCpuCycles(uint32_t(duration+f.commands.duration())));
    assert(f.commands.complete(f.state,f.eim,f.memory) == A::ok);
    assert(f.read(destination) == 0xffff); f.write(destination,0xa55a); check(f);
}
// Scissor word 470 is the first column and row not drawn, for copies and fills alike.
void scissor() {
    Fixture f; planes(f);
    // The 3 by 2 copy to x 5, y 3 cut at column 7 and row 4: two pixels of its first row.
    auto p = packet(); change(p,0x470,0x00040007);
    assert(f.append(p) == A::ok && f.commands.duration() == p.size()+3);
    f.finish();
    for (unsigned i = 0; i < 13*8; ++i)
        assert(f.read(destination+i*2) == (i == 44 ? 0u : i == 45 ? 0xf800u : 0xa55au));
    // A scissor that ends where the copy starts draws nothing.
    planes(f); p = packet(); change(p,0x470,0x00140005);
    assert(f.append(p) == A::ok && f.commands.duration() == p.size()+1);
    f.finish();
    for (unsigned i = 0; i < 13*8; ++i) assert(f.read(destination+i*2) == 0xa55a);
    // A fill of 6 by 4 at x 1, y 1 against a scissor of x 2..4, y 2..3.
    planes(f);
    assert(f.append({0x41b,0x55e0b4df,0x4b2,0x00f00a00,0x401,destination,0x402,13,0x46f,0x00020002,0x470,0x00040005,0x41f,0x07e0,
        0xc0011a00,0x00010001,0x00060004},0x200b0200) == A::ok);
    f.finish();
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 13; ++x)
            assert(f.read(destination+(y*13+x)*2) == (x >= 2 && x < 5 && y >= 2 && y < 4 ? 0x07e0u : 0xa55au));
}
// The fill routine's frame of up to four strips in one packet, and ROP3 5a (destination xor pattern).
void fills() {
    Fixture f; planes(f);
    const std::vector<uint32_t> surface{0x401,destination,0x402,13,0x46f,0,0x470,0x0014000d,0x41f,0x07e0};
    // A frame one pixel wide at x 2, y 1, 8 by 5: top, left, right, bottom.
    const std::vector<uint32_t> frame{0xc0071a00,0x00020001,0x00080001,0x00020002,0x00010003,
        0x00090002,0x00010003,0x00020005,0x00080001};
    auto with = [&](std::vector<uint32_t> p, const std::vector<uint32_t>& tail) { p.insert(p.end(),tail.begin(),tail.end()); return p; };
    assert(f.append(with(surface,frame),0x200b0200) == A::ok && f.commands.duration() == surface.size()+frame.size()+1+22);
    f.finish();
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 13; ++x) {
            const bool inside = x >= 2 && x < 10 && y >= 1 && y < 6;
            const bool edge = inside && (x == 2 || x == 9 || y == 1 || y == 5);
            assert(f.read(destination+(y*13+x)*2) == (edge ? 0x07e0u : 0xa55au));
        }
    // Two and three strips (a clipped frame), then what no producer sends.
    assert(f.append({0xc0031a00,0x00000000,0x00010001,0x00010000,0x00010001},0x200b0200) == A::ok); f.finish();
    assert(f.read(destination) == 0x07e0 && f.read(destination+2) == 0x07e0 && f.read(destination+4) == 0xa55a);
    assert(f.append({0xc0051a00,0,0x00010001,0x00010000,0x00010001,0x00020000,0x00010001},0x200b0200) == A::ok); f.finish();
    assert(f.read(destination+4) == 0x07e0);
    assert(f.append(frame,0x200a9ed4) == A::sideEffect);
    assert(f.append({0xc0031a00,0,0x00010001,0x00010000,0x00010001},0x200a9ed4) == A::sideEffect);
    assert(f.append({0xc0091a00,0,0x00010001,0,0x00010001,0,0x00010001,0,0x00010001,0,0x00010001},0x200b0200) == A::sideEffect);
    assert(f.append({0xc0021a00,0,0x00010001,0},0x200b0200) == A::sideEffect);
    assert(f.append({0xc0031a00,0,0x00010001,0x00010000,0x00000001},0x200b0200) == A::sideEffect);
    assert(f.append({0xc0031a00,0,0x00010001},0x200b0200) != A::ok);
    // Invert: 41b and 4b2 both carry 5a, the result is destination xor colour, and twice gives it back.
    const std::vector<uint32_t> invert{0x41b,0x54b4b4df,0x4b2,0x005a0a00,0x41f,0x0ff0,0xc0011a00,0x00050007,0x00020001};
    f.write(destination+(7*13+5)*2,0x1234);
    assert(f.append(invert,0x200b0200) == A::ok); f.finish();
    assert(f.read(destination+(7*13+5)*2) == (0x1234 ^ 0x0ff0) && f.read(destination+(7*13+6)*2) == (0xa55a ^ 0x0ff0));
    assert(f.read(destination+(7*13+4)*2) == 0xa55a && f.read(destination+(7*13+7)*2) == 0xa55a);
    assert(f.append({0xc0011a00,0x00050007,0x00020001},0x200b0200) == A::ok); f.finish();
    assert(f.read(destination+(7*13+5)*2) == 0x1234 && f.read(destination+(7*13+6)*2) == 0xa55a);
    // Four inverted strips in one packet.
    assert(f.append(frame,0x200b0200) == A::ok); f.finish();
    assert(f.read(destination+(1*13+2)*2) == (0x07e0 ^ 0x0ff0) && f.read(destination+(3*13+5)*2) == 0xa55a);
    // The copy helper is not known to invert, the two ROP fields must agree, and other ROPs stay refused.
    assert(f.append({0xc0011a00,0x00050007,0x00020001},0x200a9ed4) == A::sideEffect);
    assert(f.append({0x4b2,0x00f00a00,0xc0011a00,0x00050007,0x00020001},0x200b0200) == A::sideEffect);
    for (uint32_t rop : {0x55u,0x0fu,0x00u,0xffu,0xa5u})
        assert(f.append({0x41b,0x5400b4df | rop << 17,0x4b2,0x00000a00 | rop << 16,0xc0011a00,0x00050007,0x00020001},0x200b0200) == A::sideEffect);
    assert(f.append({0x41b,0x55e0b4df,0x4b2,0x00f00a00,0x41f,0x1234,0xc0011a00,0,0x00010001},0x200b0200) == A::ok); f.finish();
    assert(f.read(destination) == 0x1234);
    std::puts("PASS: fill packets of one to four rectangles, ROP3 f0 and 5a, producer and ROP guards");
}
// The software stretch's packets of n plain copies, from its two submit sites.
void stretch() {
    Fixture f; planes(f);
    auto p = packet();
    // One copy from the stretch is the copy helper's packet.
    assert(f.append(p,0x200b1790) == A::ok); f.finish(); check(f);
    planes(f); p = packet(); p.resize(20);
    // Source pixels (2,1) and (3,1) doubled along x into row 3 from x 2: four 1 by 1 copies.
    const std::vector<uint32_t> runs{0xc00b1b00,0x00020001,0x00020003,0x00010001,0x00020001,0x00030003,0x00010001,
        0x00030001,0x00040003,0x00010001,0x00030001,0x00050003,0x00010001};
    auto with = [&](std::vector<uint32_t> head, const std::vector<uint32_t>& tail) { head.insert(head.end(),tail.begin(),tail.end()); return head; };
    assert(f.append(with(p,runs),0x200b1568) == A::ok && f.commands.duration() == p.size()+runs.size()+1+4);
    f.finish();
    for (unsigned i = 0; i < 13*8; ++i) {
        const uint32_t expected = i == 41 || i == 42 ? 0 : i == 43 || i == 44 ? 0xf800 : 0xa55a;
        assert(f.read(destination+i*2) == expected);
    }
    // The next packet of the same stretch: two runs of unequal size, row 2 of the source into rows 4 and 5.
    assert(f.append({0xc0051b00,0x00020002,0x00020004,0x00030001,0x00020002,0x00020005,0x00030001},0x200b1790) == A::ok);
    f.finish();
    for (unsigned row : {4u,5u}) {
        assert(f.read(destination+(row*13+2)*2) == 0x5678 && f.read(destination+(row*13+3)*2) == 0xffff);
        assert(f.read(destination+(row*13+4)*2) == 0xabcd && f.read(destination+(row*13+5)*2) == 0xa55a);
    }
    // Refused: more than one copy from the copy helper or the fill routine, a word count that is not 3n,
    // two runs to the same place, and a source inside the span of the destinations.
    planes(f);
    assert(f.append(with(p,runs),0x200a9ed4) == A::sideEffect);
    assert(f.append(with(p,runs),0x200b0200) == A::sideEffect);
    auto bad = runs; bad[0] = 0xc00a1b00; bad.pop_back();
    assert(f.append(with(p,bad),0x200b1790) == A::sideEffect);
    bad = runs; bad[5] = 0x00020003;
    assert(f.append(with(p,bad),0x200b1790) == A::sideEffect);
    auto same = p; same[13] = destination; same[15] = 13;
    assert(f.append(with(same,{0xc0051b00,0x00000000,0x00020003,0x00010001,0x00030003,0x00050003,0x00010001}),0x200b1790) == A::sideEffect);
    bad = runs; bad.push_back(0);
    assert(f.append(with(p,bad),0x200b1790) != A::ok);
    bad = runs; bad.pop_back();
    assert(f.append(with(p,bad),0x200b1790) != A::ok);
    for (unsigned i = 0; i < 13*8; ++i) assert(f.read(destination+i*2) == 0xa55a);
    uint32_t value = f.state.engineState().producer();
    assert(f.state.access(f.eim,0x200b1790,0x16010220,4,true,value) == A::sideEffect);
    assert(f.state.access(f.eim,0x200b1794,0x16010220,4,true,value) == A::externalInput);
    std::puts("PASS: stretch packets of n plain copies from 200b1568 and 200b1790, order and overlap guards");
}
// The vertical wait reads the scan status with any batch in the ring.
void scanStatus() {
    Fixture f; planes(f);
    struct Case { std::vector<uint32_t> batch; uint32_t pc; };
    auto keyed = std::vector<uint32_t>{0x41b,0x4598b4df,0x4b2,0x00cc0a00,0x401,source,
        0x402,9,0x46b,destination,0x46c,13,0xc0051c00,0x01000005,0xf800,0,0,0,0x00020002};
    auto host = std::vector<uint32_t>{0x41b,0x5798b4df,0x4b2,0x00cc0b00,0x4b1,0x40030d04,
        0x401,destination,0x402,13,0xc0031900,0,0x00020002,0x1234f800,0x56780000};
    for (const auto& item : {Case{{0x401,destination,0x402,13,0xc0011a00,0,0x00010001},0x200b0200}, Case{packet(),0x200a9ed4},
                             Case{packet(),0x200b1790}, Case{keyed,0x200aa504}, Case{host,0x200ac300}}) {
        assert(f.append(item.batch,item.pc) == A::ok && !f.state.engineState().emptyRing());
        uint32_t scan = 0xdeadbeef;
        assert(f.state.access(f.eim,0x200adb08,0x1601054c,4,false,scan) == A::ok && (scan & ~0x400u) == 0);
        assert(f.state.access(f.eim,0x200adb14,0x1601054c,4,false,scan) == A::ok && (scan & ~0x400u) == 0);
        scan = 0xdeadbeef;
        // The status query (200ad7b0) and every other display access still wait for an empty ring.
        assert(f.state.access(f.eim,0x200ad7b0,0x1601054c,4,false,scan) == A::sequence && scan == 0xdeadbeef);
        assert(f.state.access(f.eim,0x200adb08,0x1601054c,4,true,scan) == A::sequence);
        assert(f.state.access(f.eim,0x200aeb68,0x160104d8,4,true,scan) == A::sequence);
        f.finish();
    }
    std::puts("PASS: scan status read by the vertical wait with any batch queued");
}
void executionAndReset() {
    for (bool combined : {false,true}) {
        Fixture f; planes(f);
        // Four A4 endpoint/interior results with a precomputed red-over-blue oracle.
        const uint32_t stage = 0xe05000, rgb = 0xe06000;
        for (unsigned i = 0; i < 8; ++i) f.write(stage+i*2,0x001f);
        for (unsigned i = 0; i < 8; ++i) f.write(rgb+i*2,0xf800);
        const std::vector<uint32_t> a4{0x41b,0xd798d4df,0x4b2,0x01cc1300,0x4b1,0x40057d04,
            0x488,0x80004,0x401,stage,0x402,4,0x46b,rgb,0x46c,4,
            0x418,rgb,0x419,4,0x420,4,0x421,2,0x416,0,0x46f,0,0x470,0x00020004,
            0xc0021900,0,0x00020004,0x05affa50};
        assert(f.append(a4,0x200a9a9c) == A::ok);
        const auto a4Ticks = f.commands.duration();
        const auto a4End = f.state.engineState().producer();
        auto plain = packet();
        change(plain,0x46b,stage); change(plain,0x46c,4); change(plain,0x470,0x0014000d);
        plain[plain.size()-3] = 0; plain.back() = 0x00040002;
        assert(f.append(plain) == A::ok);
        const auto copyTicks = f.commands.duration();
        const auto copyEnd = f.state.engineState().producer();
        // Register snapshots and memory-source addresses survive overwriting the ring.
        for (unsigned i = 0; i < a4.size()+plain.size(); ++i) f.write(0x150000+(60+i)*4,0xdeadbeef,4);
        assert(f.read(destination+88) == 0xa55a);
        if (!combined) {
            assert(f.state.advanceCpuCycles(uint32_t(a4Ticks)) && f.commands.complete(f.state,f.eim,f.memory) == A::ok);
            assert(f.state.engineState().consumer() == a4End && f.read(destination+88) == 0xa55a);
            assert(f.state.advanceCpuCycles(uint32_t(copyTicks-1)));
            assert(f.commands.complete(f.state,f.eim,f.memory) == A::sequence);
            assert(f.state.advanceCpuCycles(1));
        } else assert(f.state.advanceCpuCycles(uint32_t(a4Ticks+copyTicks+10)));
        assert(f.commands.complete(f.state,f.eim,f.memory) == A::ok && f.state.engineState().consumer() == copyEnd);
        const uint16_t expected[]{0x001f,0x5014,0xa00a,0xf800,0xf800,0xa00a,0x5014,0x001f};
        for (unsigned i = 0; i < 8; ++i) assert(f.read(destination+88+(i/4*13+i%4)*2) == expected[i]);
        // Older routes keep their controls, including solid submissions at the same helper PC.
        auto solid = std::vector<uint32_t>{0x41b,0x55e0b4df,0x4b2,0x00f00a00,0x41f,0x1234,
            0xc0011a00,0,0x00010001};
        assert(f.append(solid) == A::ok); f.finish(); assert(f.read(destination) == 0x1234);
        auto host = std::vector<uint32_t>{0x41b,0x5798b4df,0x4b2,0x00cc0b00,0x4b1,0x40030d04,
            0xc0031900,0,0x00020002,0x1234f800,0x56780000};
        assert(f.append(host,0x200ac300) == A::ok); f.finish();
        auto keyed = std::vector<uint32_t>{0x41b,0x4598b4df,0x4b2,0x00cc0a00,0x401,source,
            0x402,9,0x46b,destination,0x46c,13,0xc0051c00,0x01000005,0xf800,0,0,0,0x00020002};
        assert(f.append(keyed,0x200aa504) == A::ok); f.finish();
        assert(f.read(source) == 0xbeef && f.read(source+2) == 0x1234);
        assert(f.append(packet()) == A::ok);
        const auto configured = f.state;
        f.state.reset(); f.commands.reset();
        assert(f.state.advanceCpuCycles(10000) && !f.state.engineState().completed());
        assert(f.commands.complete(f.state,f.eim,f.memory) == A::sequence);
        uint32_t retained = 0;
        assert(f.memory.accessGpu(configured,f.eim,destination,2,false,retained) == A::ok && retained == 0xf800);
        Fixture cancelled;
        cancelled.write(stage,0x001f); cancelled.write(rgb,0xf800);
        cancelled.write(destination+88,0xa55a);
        assert(cancelled.append(a4,0x200a9a9c) == A::ok && cancelled.append(plain) == A::ok);
        const auto beforeReset = cancelled.state;
        cancelled.state.reset(); cancelled.commands.reset();
        assert(cancelled.state.advanceCpuCycles(10000) && !cancelled.state.engineState().completed());
        assert(cancelled.commands.complete(cancelled.state,cancelled.eim,cancelled.memory) == A::sequence);
        assert(cancelled.memory.accessGpu(beforeReset,cancelled.eim,stage,2,false,retained) == A::ok && retained == 0x001f);
        assert(cancelled.memory.accessGpu(beforeReset,cancelled.eim,destination+88,2,false,retained) == A::ok && retained == 0xa55a);
    }
}
void privateCapture() {
    const char* sourcePath = std::getenv("FROND_ZODIAC_PLAIN_SOURCE");
    const char* packetPath = std::getenv("FROND_ZODIAC_PLAIN_PACKET");
    assert((sourcePath && packetPath) || (!sourcePath && !packetPath));
    if (!sourcePath) return;
    auto bytes = [](const char* path) {
        std::ifstream file(path,std::ios::binary); assert(file);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),{});
    };
    const auto sourceBytes = bytes(sourcePath), captured = bytes(packetPath);
    assert(sourceBytes.size() == 728 && captured.size() == 72);
    Fixture f;
    std::vector<uint32_t> p;
    for (size_t i = 0; i < captured.size(); i += 4)
        p.push_back(uint32_t(captured[i])|uint32_t(captured[i+1])<<8|uint32_t(captured[i+2])<<16|uint32_t(captured[i+3])<<24);
    assert(f.append({0x46c,16},0x200b0200) == A::ok); f.finish();
    for (unsigned y = 0; y < 26; ++y)
        for (unsigned x = 0; x < 28; ++x) f.write(0xf50980+y*32+x,sourceBytes[y*28+x],1);
    for (unsigned y = 0; y < 28; ++y)
        for (unsigned x = 104; x < 122; ++x) f.write(0x100000+(y*480+x)*2,0xffff);
    assert(f.append(p) == A::ok && f.commands.duration() == 383); f.finish();
    unsigned changed = 0;
    for (unsigned y = 0; y < 28; ++y)
        for (unsigned x = 104; x < 122; ++x) {
            uint32_t expected = 0xffff;
            if (y < 26 && x >= 106 && x < 120) {
                const auto at = y*28+(x-106)*2;
                expected = uint32_t(sourceBytes[at])|uint32_t(sourceBytes[at+1])<<8;
                if (expected != 0xffff) ++changed;
            }
            assert(f.read(0x100000+(y*480+x)*2) == expected);
        }
    assert(changed == 145);
    std::puts("PASS: private native 18-word plain copy, every364 display pixels and surrounding bounds, 145 nonwhite");
}
}
int main() {
    geometry(); guards(); scissor(); fills(); stretch(); scanStatus(); executionAndReset(); privateCapture();
    std::puts("PASS: plain RGB565 copy geometry, bounds, atomic append, queued A4 execution-time source, deadlines, reset and routes");
}
