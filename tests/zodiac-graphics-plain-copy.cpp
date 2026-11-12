#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_commands.h"
#include "machine_state_archive.h"

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
        {21,0x00090001},{22,0x1fff0000},{22,0x00002000},{23,0},{23,0x00010000},{23,0xffff0001}}) {
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
bool refused(const Fixture& f, const char* what, uint32_t value) {
    const auto& r = f.commands.refusal();
    return r.what && std::string(r.what) == what && r.value == value;
}
constexpr uint32_t scratch = 0xf51000, screen = 0x100000;
bool bitmapBit(unsigned x, unsigned y) { return ((x * 7 + y * 13 + x * y) % 5) < 2; }
// The Date Book capture's ring words: a 9 by 11 bitmap on the scratch surface drawn 18 by 22 at x 2, y 296.
std::vector<uint32_t> dateBookStretch() {
    return {0x4a1,0,0x41b,0xd598b4df,0x472,0x3f,0x4b2,0x01cc1200,0x4b1,0x40037d04,
        0x46f,0x01280002,0x470,0x013e0014,0x401,screen,0x402,0x1e0,0x46b,scratch,0x46c,0x140,0x488,0x02020009,
        0x416,4,0x4020c419,0x140,scratch,0x00020410,0x13,0x16,0x00800080,0x00020420,0x13,0x16,0x00800080,
        0x00020464,0,0x00020128,0x00120016};
}
void stretchPlanes(Fixture& f) {
    // The bitmap as its one bit upload leaves it, black on white, with red where it must not be read.
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 0; x < 10; ++x)
            f.write(scratch+(y*0x140+x)*2, x < 9 && y < 11 ? (bitmapBit(x,y) ? 0 : 0xffff) : 0xf800);
    for (unsigned y = 290; y < 320; ++y) for (unsigned x = 0; x < 64; ++x) f.write(screen+(y*480+x)*2,0xa55a);
}
// The chip's stretch blit from 200b0b14 at ratio 80 hex: each source pixel twice each way, into memory.
void stretchBlit() {
    Fixture f; stretchPlanes(f);
    auto p = dateBookStretch();
    assert(f.append(p,0x200b0b14) == A::ok && f.commands.duration() == p.size()+1+18*22);
    f.finish();
    for (unsigned y = 290; y < 320; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            const bool inside = x >= 2 && x < 20 && y >= 296 && y < 318;
            const uint32_t expected = !inside ? 0xa55au : bitmapBit((x-2)/2,(y-296)/2) ? 0u : 0xffffu;
            assert(f.read(screen+(y*480+x)*2) == expected);
        }
    // A scissor one pixel inside each edge keeps every pixel on its own source pixel.
    stretchPlanes(f); p = dateBookStretch(); p[11] = 0x01290003; p[13] = 0x013d0013;
    assert(f.append(p,0x200b0b14) == A::ok && f.commands.duration() == p.size()+1+16*20);
    f.finish();
    for (unsigned y = 290; y < 320; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            const bool inside = x >= 3 && x < 19 && y >= 297 && y < 317;
            const uint32_t expected = !inside ? 0xa55au : bitmapBit((x-2)/2,(y-296)/2) ? 0u : 0xffffu;
            assert(f.read(screen+(y*480+x)*2) == expected);
        }
    // A ratio outside 10 to 1ff hex either way stops and the stop names it. So do repeated words that disagree
    // and other state.
    for (uint32_t ratio : {0x0080000fu,0x00800200u,0x000f0080u,0x02000080u,0u}) {
        p = dateBookStretch(); p[32] = p[36] = ratio;
        assert(f.append(p,0x200b0b14) == A::sideEffect && refused(f,"stretch-ratio",ratio));
    }
    p = dateBookStretch(); p[36] = 0x00800100;
    assert(f.append(p,0x200b0b14) == A::sideEffect && refused(f,"stretch-words",0));
    for (auto item : {std::pair<unsigned,uint32_t>{25,0},{27,0x141},{28,scratch+2},{30,0x12},{31,0x15},{34,0x14}}) {
        p = dateBookStretch(); p[item.first] = item.second;
        assert(f.append(p,0x200b0b14) == A::sideEffect && refused(f,"stretch-words",0));
    }
    for (auto item : {std::pair<unsigned,uint32_t>{3,0x4598b4df},{3,0xd598b4de},{3,0xd798d4df},{3,0x5398b4df},{7,0x00cc1200},{9,0x40030d04},{23,4}}) {
        p = dateBookStretch(); p[item.first] = item.second;
        assert(f.append(p,0x200b0b14) == A::sideEffect && refused(f,"stretch-state",p[item.first-1]));
    }
    // Smooth (488 bit 0 clear) filters, and how is not known.
    p = dateBookStretch(); p[23] = 0x02020008;
    assert(f.append(p,0x200b0b14) == A::sideEffect && refused(f,"stretch-smooth",0x00800080));
    // The stretch words from any other producer, and other packets from this one, are refused.
    for (uint32_t pc : {0x200a9ed4u,0x200b0200u,0x200b1790u}) assert(f.append(dateBookStretch(),pc) != A::ok);
    p = dateBookStretch(); p.insert(p.end(),{0xc0011a00,0,0x00010001});
    assert(f.append(p,0x200b0b14) != A::ok);
    p = dateBookStretch(); p.resize(p.size()-4);
    assert(f.append(p,0x200b0b14) != A::ok);
    // The source must lie in memory and clear of what is written.
    p = dateBookStretch(); p[19] = p[28] = screen+296*960;
    assert(f.append(p,0x200b0b14) == A::sideEffect);
    assert(f.read(screen+(297*480+3)*2) == (bitmapBit(0,0) ? 0u : 0xffffu) && f.read(screen+(296*480+2)*2) == 0xa55a);
}
// The bitmap routine's erase and mask: stretch to a second scratch surface, then a keyed copy from it that
// paints the back colour (ROP f0, solid brush) where the source is the key (control 4) or is not (5).
void keyedModes() {
    const uint32_t second = 0xe03000;
    for (uint32_t control : {0x01000004u,0x01000005u}) {
        Fixture f; stretchPlanes(f);
        auto p = dateBookStretch();
        p[11] = 0; p[13] = 0x00160012; p[15] = second; p[17] = 0x20; p[39] = 0;
        assert(f.append(p,0x200b0b14) == A::ok);
        const std::vector<uint32_t> keyed{0x488,4,0x4b1,0x40030d04,0x4b2,0x00f00a00,0x41b,0x45e0b4df,0x41f,0x07e0,
            0x401,screen,0x402,0x1e0,0x46b,second,0x46c,0x20,0x46f,0,0x470,0x014001e0,
            0xc0051c00,control,0xffff,0,0,0x00280128,0x00120016};
        assert(f.append(keyed,0x200aa504) == A::ok);
        assert(f.state.advanceCpuCycles(4000) && f.commands.complete(f.state,f.eim,f.memory) == A::ok);
        for (unsigned y = 0; y < 22; ++y)
            for (unsigned x = 0; x < 18; ++x) {
                assert(f.read(second+(y*0x20+x)*2) == (bitmapBit(x/2,y/2) ? 0u : 0xffffu));
                const bool painted = bitmapBit(x/2,y/2) == (control == 0x01000005);
                assert(f.read(screen+((296+y)*480+40+x)*2) == (painted ? 0x07e0u : 0xa55au));
            }
        assert(f.read(screen+(296*480+39)*2) == 0xa55a && f.read(screen+(296*480+58)*2) == 0xa55a);
    }
    Fixture f; stretchPlanes(f);
    std::vector<uint32_t> keyed{0x4b2,0x00cc0a00,0x41b,0x4598b4df,0x401,screen,0x402,0x1e0,0x46b,scratch,0x46c,0x140,0x46f,0,0x470,0x014001e0,
        0xc0051c00,0x01000004,0xffff,0,0,0x00280128,0x00090005};
    // Control 4 with ROP cc copies the pixels that match the key.
    assert(f.append(keyed,0x200aa504) == A::ok); f.finish();
    for (unsigned x = 0; x < 9; ++x) assert(f.read(screen+(296*480+40+x)*2) == (bitmapBit(x,0) ? 0xa55au : 0xffffu));
    // Other compare words and other ROPs stop by name.
    for (uint32_t control : {0x01000001u,0x01000000u,0x02000405u,0x01000105u}) {
        keyed[17] = control;
        assert(f.append(keyed,0x200aa504) == A::sideEffect && refused(f,"keyed-control",control));
    }
    keyed[17] = 0x01000005; keyed[3] = 0x45b4b4df;
    assert(f.append(keyed,0x200aa504) == A::sideEffect && refused(f,"keyed-copy-41b",0x45b4b4df));
    auto p = packet(); p[1] = 0x5532b4df;
    assert(f.append(p) == A::sideEffect && refused(f,"plain-copy-41b",0x5532b4df));
    // The line packet's third point must repeat its end (lines() has the rest).
    assert(f.append({0xc0021500,0x00010001,0x00050005,0},0x200affe4) == A::sideEffect && refused(f,"line-points",0));
    assert(f.append({0xc0021500,0x00010001,0x00050005,0},0x200b0200) == A::sideEffect);
}
// The brush uploads of 200aedd0 land on the brush surface as sent. A fill with a pattern brush type stops.
void brushUpload() {
    const uint32_t brush = 0xe05000;
    const uint8_t rows[8]{0xaa,0x55,0x88,0x22,0xdd,0x77,0x01,0x80};
    const uint32_t d0 = rows[0]|rows[1]<<8|rows[2]<<16|uint32_t(rows[3])<<24, d1 = rows[4]|rows[5]<<8|rows[6]<<16|uint32_t(rows[7])<<24;
    {
        Fixture f;
        for (unsigned i = 0; i < 48; ++i) f.write(brush-8+i,0xee,1);
        const std::vector<uint32_t> mono{0x423,brush,0x401,brush,0x402,0x10,0x4b2,0x00cc0b00,0x41b,0x5798b303,0x4b1,0x40036003,
            0xc0011a00,0,0x00100001,0x000704f0,d0,d0>>8,d0>>16,d0>>24,d1,d1>>8,d1>>16,d1>>24};
        assert(f.append(mono,0x200af1fc) == A::ok && f.commands.duration() == mono.size()+1+16);
        f.finish();
        // Row i of the pattern is the low byte of word i.
        for (unsigned i = 0; i < 8; ++i) assert(f.read(brush+i*4,1) == rows[i]);
        assert(f.read(brush,4) == d0 && f.read(brush+12,4) == d0>>24 && f.read(brush+28,4) == d1>>24);
        assert(f.read(brush-1,1) == 0xee && f.read(brush+32,1) == 0xee);
        // The driver puts the surface back and fills with brush type 0 or 1: a stop that names the type.
        for (uint32_t type : {0u,1u,6u,7u,10u}) {
            const uint32_t gmc = 0x55e0340f | type << 4;
            assert(f.append({0x401,destination,0x402,13,0x4b2,0x00f00a00,0x41b,gmc,0x4b1,0x00030004u|type<<8,
                0xc0011a00,0x00010001,0x00020002},0x200b0200) == A::sideEffect && refused(f,"pattern-brush",type));
        }
        // A 32 by 1 brush is four words.
        std::vector<uint32_t> wide{0x41b,0x5798b303,0x402,8,0xc0011a00,0,0x00080001,0x000304f0,d1,d1>>8,d1>>16,d1>>24};
        assert(f.append(wide,0x200af1fc) == A::ok); f.finish();
        for (unsigned i = 0; i < 4; ++i) assert(f.read(brush+i*4,1) == rows[4+i]);
        assert(f.read(brush+16,1) == rows[4]);
        // Wrong surface, pitch, state, data header, length and producer.
        auto bad = mono; bad[3] = brush+0x80;
        assert(f.append(bad,0x200af1fc) == A::sideEffect && refused(f,"brush-state",0x401));
        bad = mono; bad[5] = 0x20;
        assert(f.append(bad,0x200af1fc) == A::sideEffect && refused(f,"brush-state",0x402));
        bad = mono; bad[9] = 0x5798b30b;
        assert(f.append(bad,0x200af1fc) == A::sideEffect && refused(f,"brush-state",0x41b));
        bad = mono; bad[15] = 0x000784f0;
        assert(f.append(bad,0x200af1fc) == A::sideEffect && refused(f,"brush-packet",0x00100001));
        bad = mono; bad[14] = 0x00100002;
        assert(f.append(bad,0x200af1fc) == A::sideEffect);
        bad = mono; bad.pop_back(); assert(f.append(bad,0x200af1fc) != A::ok);
        bad = mono; bad.push_back(0); assert(f.append(bad,0x200af1fc) != A::ok);
        assert(f.append(mono,0x200af448) != A::ok && f.append(mono,0x200b0200) != A::ok);
    }
    Fixture f;
    std::vector<uint32_t> colour{0x423,brush,0x401,brush,0x402,0x40,0x4b2,0x00cc0b00,0x41b,0x5798b4f3,0x4b1,0x40036f04,
        0xc0011a00,0,0x00400001,0x001f84f0};
    for (uint32_t i = 0; i < 32; ++i) colour.push_back((0x1000+i*2) | (0x1001+i*2) << 16);
    assert(f.append(colour,0x200af448) == A::ok && f.commands.duration() == colour.size()+1+64);
    f.finish();
    // 64 pixels of 16 bits, row after row.
    for (uint32_t i = 0; i < 64; ++i) assert(f.read(brush+i*2) == 0x1000+i);
    assert(f.append(colour,0x200af1fc) != A::ok);
    assert(f.append({0x401,destination,0x402,13,0x4b2,0x00f00a00,0x41b,0x55e0b4af,0x4b1,0x40030a04,
        0xc0011a00,0x00010001,0x00020002},0x200b0200) == A::sideEffect && refused(f,"pattern-brush",10));
}
// Scissor word 470 is the first column and row not drawn, for copies and fills alike.
void hostSourceType() {
    // RGB565 rows go in with 4b1 source type 3 or 0, as the ROM leaves it. Other types stop with the value.
    for (uint32_t datatype : {0x40030d04u,0x40000d04u,0x40010d04u,0x40037d04u}) {
        Fixture f;
        const std::vector<uint32_t> host{0x41b,0x5798b4df,0x4b2,0x00cc0b00,0x4b1,datatype,
            0xc0031900,0,0x00020002,0x1234f800,0x56780000};
        if (datatype == 0x40030d04 || datatype == 0x40000d04) {
            assert(f.append(host,0x200ac300) == A::ok); f.finish();
        } else assert(f.append(host,0x200ac300) == A::sideEffect && refused(f,"host-upload-state",datatype));
    }
}
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
    // A run of no width or no height, as a shrink sends, draws nothing; the others in its packet are drawn.
    planes(f);
    assert(f.append({0xc0081b00,0x00020002,0x00020004,0x00000001,0x00020002,0x00020005,0x00030000,
                     0x00020002,0x00020006,0x00030001},0x200b1790) == A::ok);
    f.finish();
    for (unsigned i = 0; i < 13*8; ++i) {
        const uint32_t expected = i == 6*13+2 ? 0x5678 : i == 6*13+3 ? 0xffff : i == 6*13+4 ? 0xabcd : 0xa55a;
        assert(f.read(destination+i*2) == expected);
    }
    // From the copy helper it stays refused.
    assert(f.append(with(p,{0xc0021b00,0x00020002,0x00020004,0x00000001}),0x200a9ed4) == A::sideEffect);
    // Refused: more than one copy from the copy helper or the fill routine, a word count that is not 3n,
    // two runs to the same place, and a run's source on another run's destination.
    planes(f);
    assert(f.append(with(p,runs),0x200a9ed4) == A::sideEffect);
    assert(f.append(with(p,runs),0x200b0200) == A::sideEffect);
    auto bad = runs; bad[0] = 0xc00a1b00; bad.pop_back();
    assert(f.append(with(p,bad),0x200b1790) == A::sideEffect);
    bad = runs; bad[5] = 0x00020003;
    assert(f.append(with(p,bad),0x200b1790) == A::sideEffect && refused(f,"stretch-runs-overlap",1));
    auto same = p; same[13] = destination; same[15] = 13;
    assert(f.append(with(same,{0xc0051b00,0x00000000,0x00020003,0x00010001,0x00020003,0x00050003,0x00010001}),0x200b1790) == A::sideEffect &&
           refused(f,"stretch-source-overlap",1));
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
// Source and destination side by side on one surface share no pixel and are drawn. Shared pixels stop.
void sideBySide() {
    Fixture f; planes(f);
    auto pixel = [](unsigned x, unsigned y) { return destination + (y * 13 + x) * 2; };
    f.write(pixel(0,0),0x1111); f.write(pixel(1,0),0x2222); f.write(pixel(0,1),0x3333); f.write(pixel(1,1),0x4444);
    f.write(pixel(3,3),0x5555);
    auto p = packet(); p[13] = destination; p[15] = 13;
    // One 2 by 2 copy from (0,0) to (3,0): its rows lie between the source's in memory.
    p[21] = 0; p[22] = 0x00030000; p[23] = 0x00020002;
    assert(f.append(p) == A::ok); f.finish();
    assert(f.read(pixel(3,0)) == 0x1111 && f.read(pixel(4,0)) == 0x2222);
    assert(f.read(pixel(3,1)) == 0x3333 && f.read(pixel(4,1)) == 0x4444 && f.read(pixel(5,0)) == 0xa55a);
    p[22] = 0x00010001;
    assert(f.append(p) == A::sideEffect && refused(f,"copy-overlap",0));
    // Two stretch runs, (0,0) to (2,3) and (3,3) to (5,3), with the sources' span over the first destination.
    p.resize(20);
    auto runs = p;
    runs.insert(runs.end(),{0xc0051b00,0x00000000,0x00020003,0x00010001,0x00030003,0x00050003,0x00010001});
    assert(f.append(runs,0x200b1790) == A::ok); f.finish();
    assert(f.read(pixel(2,3)) == 0x1111 && f.read(pixel(5,3)) == 0x5555 && f.read(pixel(4,3)) == 0xa55a);
    // The chip's stretch blit with its source beside the destination on the screen, then on it.
    stretchPlanes(f);
    for (unsigned y = 0; y < 11; ++y)
        for (unsigned x = 0; x < 9; ++x) f.write(screen+((296+y)*480+40+x)*2, bitmapBit(x,y) ? 0 : 0xffff);
    auto blit = dateBookStretch(); blit[19] = blit[28] = screen+(296*480+40)*2; blit[21] = blit[27] = 480;
    assert(f.append(blit,0x200b0b14) == A::ok); f.finish();
    for (unsigned y = 296; y < 318; ++y)
        for (unsigned x = 2; x < 20; ++x) assert(f.read(screen+(y*480+x)*2) == (bitmapBit((x-2)/2,(y-296)/2) ? 0u : 0xffffu));
    blit = dateBookStretch(); blit[19] = blit[28] = screen+(296*480+10)*2; blit[21] = blit[27] = 480;
    assert(f.append(blit,0x200b0b14) == A::sideEffect && refused(f,"stretch-overlap",0x00020128));
    std::puts("PASS: copies, stretch runs and the stretch blit side by side on one surface, shared pixels refused");
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
        // The backlight level (4f0) reads and writes with drawing queued.
        uint32_t level = 0xdeadbeef;
        assert(f.state.access(f.eim,0x20086d12,0x160104f0,4,false,level) == A::ok && level != 0xdeadbeef);
        uint32_t set = (level & 0xf800ffffu) | 0x40000000u | 0x01230000u;
        assert(f.state.access(f.eim,0x200869f0,0x160104f0,4,true,set) == A::ok);
        assert(f.state.access(f.eim,0x20086d12,0x160104f0,4,false,level) == A::ok && level == set);
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

// The stretch blit at ratio 80 hex with the GMC another operation left: the ROM changes its GMC shadow for the
// stretch but doesn't always send it. DP_MIX, sent after it, holds the stretch's source and ROP.
void stretchStaleGmc() {
    for (uint32_t gmc : {0x5798b4dfu,0x5598b4dfu,0x55e0b4dfu}) {
        for (bool direct : {true,false}) {
            Imx1GraphicsCommands::directFills = direct;
            Fixture f; stretchPlanes(f);
            auto p = dateBookStretch(); p[3] = gmc;
            const auto status = f.append(p,0x200b0b14);
            if (status != A::ok) std::fprintf(stderr,"gmc %08x: %d %s %08x\n",gmc,int(status),
                f.commands.refusal().what ? f.commands.refusal().what : "-",f.commands.refusal().value);
            assert(status == A::ok);
            f.finish();
            for (unsigned y = 290; y < 320; ++y)
                for (unsigned x = 0; x < 64; ++x) {
                    const bool inside = x >= 2 && x < 20 && y >= 296 && y < 318;
                    const uint32_t expected = !inside ? 0xa55au : bitmapBit((x-2)/2,(y-296)/2) ? 0u : 0xffffu;
                    assert(f.read(screen+(y*480+x)*2) == expected);
                }
        }
        // The same GMC sent after DP_MIX sets DP_MIX's source and ROP fields again: refused.
        Fixture f; stretchPlanes(f);
        auto p = dateBookStretch(); p.erase(p.begin()+2,p.begin()+4); p.insert(p.begin()+8,{0x41b,gmc});
        assert(f.append(p,0x200b0b14) == A::sideEffect && refused(f,"stretch-state",0x4b2));
    }
    Imx1GraphicsCommands::directFills = true;
    std::puts("PASS: stretch blit after a host upload, copy or fill GMC, DP_MIX sent last");
}
// The stretch blit's words for an output of width by height at the ratios (x low, y high) into (x, y).
std::vector<uint32_t> ratioStretch(uint32_t ratio, uint32_t width, uint32_t height, uint32_t x = 2, uint32_t y = 296) {
    auto p = dateBookStretch();
    p[30] = p[34] = width+1; p[31] = p[35] = height; p[32] = p[36] = ratio;
    p[39] = x << 16 | y; p[40] = width << 16 | height;
    // The scissor from (2, 296) to (72, 320).
    p[13] = 0x01400048;
    return p;
}
// Destination pixel (dx, dy) of the stretch reads source (dx * ratio >> 8, dy * ratio >> 8) from the origin.
void stretchRatios() {
    struct Case { uint32_t ratio, width, height; };
    // Spout's 46 hex both ways, a shrink, unequal ratios, the ends of the range and 1:1.
    for (auto c : {Case{0x00460046,18,22}, Case{0x01800180,6,7}, Case{0x00800140,7,22}, Case{0x01ff0010,64,6},
                   Case{0x00100100,9,24}, Case{0x01000100,9,11}}) {
        const uint32_t rx = c.ratio & 0xffff, ry = c.ratio >> 16;
        for (bool clip : {false,true})
            for (bool direct : {true,false}) {
                Imx1GraphicsCommands::directFills = direct;
                Fixture f; stretchPlanes(f);
                for (unsigned y = 290; y < 320; ++y) for (unsigned x = 64; x < 72; ++x) f.write(screen+(y*480+x)*2,0xa55a);
                auto p = ratioStretch(c.ratio,c.width,c.height);
                // A scissor one pixel inside the left and top edges.
                if (clip) p[11] = 0x01290003;
                assert(f.append(p,0x200b0b14) == A::ok);
                f.finish();
                for (unsigned y = 290; y < 320; ++y)
                    for (unsigned x = 0; x < 72; ++x) {
                        const unsigned left = clip ? 3 : 2, top = clip ? 297 : 296;
                        const bool inside = x >= left && x < 2+c.width && y >= top && y < 296+c.height;
                        uint32_t expected = 0xa55a;
                        if (inside) {
                            const unsigned sx = (x-2)*rx/256, sy = (y-296)*ry/256;
                            assert(sx < 9 && sy < 11);
                            expected = bitmapBit(sx,sy) ? 0u : 0xffffu;
                        }
                        if (f.read(screen+(y*480+x)*2) != expected) {
                            std::fprintf(stderr,"ratio %08x clip %d direct %d at %u,%u: %04x, not %04x\n",c.ratio,clip,direct,x,y,
                                         f.read(screen+(y*480+x)*2),expected);
                            std::abort();
                        }
                    }
            }
    }
    Imx1GraphicsCommands::directFills = true;
    // A source that runs past the surface's memory is refused before anything is drawn.
    Fixture f; stretchPlanes(f);
    auto p = ratioStretch(0x00100010,16,16); p[19] = p[28] = 0xfffffffe;
    assert(f.append(p,0x200b0b14) != A::ok);
    std::puts("PASS: stretch blit at ratios 10 to 1ff hex each way, clipped or not, on both CPU paths");
}
constexpr uint32_t lineSurface = 0xe08000, lineBackground = 0x3333;
// The fill state the line routine sends (2008ae14 to 200afd00): solid brush, ROP f0, scissor on.
void linePlanes(Fixture& f) { for (unsigned i = 0; i < 128*128; ++i) f.write(lineSurface+i*2,lineBackground); }
std::vector<uint32_t> lineState(uint32_t colour) {
    return {0x41f,colour,0x401,lineSurface,0x402,128,0x46f,0,0x470,0x00800080};
}
uint32_t point(uint32_t x, uint32_t y) { return x | y << 16; }
// The pixels of a segment, both ends, as X's zero-width line (mizerline.c) draws them with the bias the Radeon
// driver sets for its hardware lines: octants 5 to 8, those with y increasing.
std::vector<std::pair<int,int>> linePixels(int x0, int y0, int x1, int y1) {
    std::vector<std::pair<int,int>> out;
    const int adx = std::abs(x1-x0), ady = std::abs(y1-y0), sx = x1 < x0 ? -1 : 1, sy = y1 < y0 ? -1 : 1;
    const int bias = y1 > y0 ? 1 : 0;
    // mizerline takes adx > ady as x-major; equal lengths step both every time, so either way draws the same.
    const bool xMajor = adx > ady;
    const int major = xMajor ? adx : ady, minor = xMajor ? ady : adx;
    const int e1 = minor*2, e3 = minor*2-major*2-e1;
    int e = e1-major-bias-e1, x = x0, y = y0;
    for (int n = 0; n < major; ++n) {
        out.push_back({x,y});
        e += e1;
        if (e >= 0) { if (xMajor) y += sy; else x += sx; e += e3; }
        if (xMajor) x += sx; else y += sy;
    }
    out.push_back({x,y});
    assert(x == x1 && y == y1);
    return out;
}
void lines() {
    struct Segment { int x0, y0, x1, y1; };
    // With ties (half way between two pixels) in each of the 8 directions: 4 across and 1 or 3 down, and the
    // y-major ones, both ways.
    const std::vector<Segment> segments{{10,10,15,12},{40,30,33,21},{5,50,25,50},{60,40,60,5},{70,10,80,20},
        {90,90,90,90},{120,3,101,6},{3,120,8,101},
        {20,60,24,61},{24,64,20,65},{20,68,24,67},{24,72,20,71},{30,60,31,64},{36,60,35,64},{40,64,41,60},{46,64,45,60},
        {50,60,54,63},{54,66,50,69},{60,70,64,67},{70,60,73,64},{80,64,77,60},{11,80,17,82},{30,80,24,82}};
    for (bool clip : {false,true})
        for (bool direct : {true,false}) {
            Imx1GraphicsCommands::directFills = direct;
            Fixture f; linePlanes(f);
            auto batch = lineState(0x07e0);
            // The scissor from 12,8 to 100,100 (exclusive).
            if (clip) batch.insert(batch.end(),{0x46f,0x0008000c,0x470,0x00640064});
            unsigned drawn = 0;
            std::vector<uint32_t> expected(128*128,lineBackground);
            for (auto s : segments) {
                batch.insert(batch.end(),{0xc0021500,point(s.x0,s.y0),point(s.x1,s.y1),point(s.x1,s.y1)});
                for (auto px : linePixels(s.x0,s.y0,s.x1,s.y1))
                    if (!clip || (px.first >= 12 && px.first < 100 && px.second >= 8 && px.second < 100)) {
                        expected[px.second*128+px.first] = 0x07e0;
                        ++drawn;
                    }
            }
            assert(f.append(batch,0x200affe4) == A::ok && f.commands.duration() == batch.size()+1+drawn);
            f.finish();
            for (unsigned i = 0; i < 128*128; ++i)
                if (f.read(lineSurface+i*2) != expected[i]) {
                    std::fprintf(stderr,"line clip %d direct %d at %u,%u: %04x, not %04x\n",clip,direct,i%128,i/128,
                                 f.read(lineSurface+i*2),expected[i]);
                    std::abort();
                }
        }
    Imx1GraphicsCommands::directFills = true;
    Fixture f; linePlanes(f);
    auto refuse = [&](std::vector<uint32_t> words, const char* what, uint32_t value, uint32_t pc = 0x200affe4) {
        auto batch = lineState(0xf800);
        batch.insert(batch.end(),words.begin(),words.end());
        assert(f.append(batch,pc) == A::sideEffect && refused(f,what,value));
        assert(f.read(lineSurface+(10*128+10)*2) == lineBackground);
    };
    // A third point that isn't the end is refused, so is a packet of another length, the packet from another
    // producer and ROP 5a.
    refuse({0xc0021500,point(10,10),point(16,12),point(16,13)},"line-points",point(16,13));
    refuse({0xc0031500,point(10,10),point(16,12),point(16,12),point(16,12)},"line-packet",0xc0031500);
    refuse({0xc0021500,point(10,10),point(16,12),point(16,12)},"line-packet",0xc0021500,0x200b0200);
    refuse({0x41b,0x54b4b4df,0x4b2,0x005a0a00,0xc0021500,point(10,10),point(13,10),point(13,10)},"line-rop",0x5a);
    // Past the surface's pitch with the scissor off is refused as a fill is.
    auto batch = lineState(0xf800); batch.insert(batch.end(),{0x41b,0x55e0b4d7,0xc0021500,point(120,10),point(130,10),point(130,10)});
    assert(f.append(batch,0x200affe4) == A::sideEffect);
    std::puts("PASS: line packets from 200affe4, any direction, ties as the Radeon bias, scissored, other forms refused");
}
// A queued batch of 45 degree lines holds a fill for every pixel. Saved mid-batch it loads, and finishes with the
// pixels of a run that never saved.
void linesSaved() {
    auto build = [] {
        auto batch = lineState(0x07e0);
        unsigned fills = 0;
        for (unsigned pass = 0; pass < 3; ++pass) {
            batch.insert(batch.end(),{0x41f,0x07e0u+pass*0x1111});
            for (unsigned k = 0; k < 64; ++k) {
                const unsigned end = 127-k;
                batch.insert(batch.end(),{0xc0021500,point(0,k),point(end,127),point(end,127)});
                fills += 128-k;
            }
        }
        assert(fills > 16384);
        return batch;
    };
    Fixture reference; linePlanes(reference);
    assert(reference.append(build(),0x200affe4) == A::ok);
    reference.finish();
    Fixture saved; linePlanes(saved);
    assert(saved.append(build(),0x200affe4) == A::ok);
    std::vector<uint8_t> bytes[2];
    StateWriter configurationOut(bytes[0]); saved.state.state(configurationOut);
    StateWriter commandsOut(bytes[1]); saved.commands.state(commandsOut);
    Fixture loaded; linePlanes(loaded);
    StateReader configurationIn(bytes[0].data(),bytes[0].size()); loaded.state.state(configurationIn);
    StateReader commandsIn(bytes[1].data(),bytes[1].size()); loaded.commands.state(commandsIn);
    if (!configurationIn.complete() || !commandsIn.complete()) {
        std::fprintf(stderr,"line batch state: %s %s\n",configurationIn.detail() ? configurationIn.detail() : "ok",
                     commandsIn.detail() ? commandsIn.detail() : "ok");
        std::abort();
    }
    loaded.finish();
    for (unsigned i = 0; i < 128*128; ++i) assert(loaded.read(lineSurface+i*2) == reference.read(lineSurface+i*2));
    assert(loaded.read(lineSurface+(5*128+5)*2) != lineBackground);
    std::puts("PASS: a queued batch of over 16384 line fills saves, loads and finishes the same");
}
// A plain copy past its surface's pitch with the scissor on draws what the scissor keeps, as the software
// stretch's runs need on a small surface.
void clippedPastPitch() {
    for (bool direct : {true,false}) {
        Imx1GraphicsCommands::directFills = direct;
        Fixture f; planes(f);
        auto p = packet(); p[21] = 0x00020001; p[22] = 0x000b0003; p[23] = 0x00030002;
        assert(f.append(p,0x200b1790) == A::ok); f.finish();
        for (unsigned i = 0; i < 13*8; ++i) {
            uint32_t expected = 0xa55a;
            if (i == 3*13+11) expected = 0;
            if (i == 3*13+12) expected = 0xf800;
            if (i == 4*13+11) expected = 0x5678;
            if (i == 4*13+12) expected = 0xffff;
            assert(f.read(destination+i*2) == expected);
        }
        // A scissor past the pitch still stops it.
        change(p,0x470,0x00140014);
        assert(f.append(p,0x200b1790) == A::sideEffect);
    }
    Imx1GraphicsCommands::directFills = true;
    std::puts("PASS: plain copy past the pitch, trimmed by the scissor");
}
// TwGfxBlendBlt's batch as 200a9414 sends it (flag 1): state, source 2 (419/418, 420/421, 416) and one paint packet.
constexpr uint32_t blendDestination = 0xe0a000, blendSource = 0xe0c000;
std::vector<uint32_t> blendBatch(uint32_t alpha, uint32_t sx, uint32_t sy, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 uint32_t scissor = 0x00100010) {
    return {0x41b,0xd798d4df,0x4b2,0x01cc1300,0x4b1,0x40057d04,0x488,4,0x484,alpha,
        0x401,blendDestination,0x402,16,0x46b,blendSource,0x46c,12,0x46f,0,0x470,scissor,
        0x4020c419,12,blendSource,0x40210c20,w,h,0x416,sx << 16 | sy,
        0xc0011a00,x << 16 | y,w << 16 | h};
}
uint16_t blendDestinationPixel(unsigned x, unsigned y) { return uint16_t(0x8410 + x * 0x0841 + y * 0x1003); }
uint16_t blendSourcePixel(unsigned x, unsigned y) { return uint16_t(0x1f00 ^ (x * 0x2945 + y * 0x0c63)); }
void blendPlanes(Fixture& f) {
    for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) f.write(blendDestination+(y*16+x)*2,blendDestinationPixel(x,y));
    for (unsigned y = 0; y < 12; ++y) for (unsigned x = 0; x < 12; ++x) f.write(blendSource+(y*12+x)*2,blendSourcePixel(x,y));
}
// Each 5/6/5 channel: (d * (15 - a) + s * a) / 15 rounded down, with its own a (red 16-19, green 8-11, blue 0-3).
uint16_t blendExpected(uint16_t d, uint16_t s, uint32_t alpha) {
    const unsigned r = (alpha >> 16) & 15, g = (alpha >> 8) & 15, b = alpha & 15;
    const unsigned dr = d >> 11, dg = (d >> 5) & 63, db = d & 31, sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
    return uint16_t(((dr*(15-r)+sr*r)/15) << 11 | ((dg*(15-g)+sg*g)/15) << 5 | (db*(15-b)+sb*b)/15);
}
void blend() {
    struct Case { uint32_t alpha, sx, sy, x, y, w, h, scissor; };
    // Alpha 0 keeps the destination, f0f0f copies, mixed alphas per channel; with a source origin and with the
    // scissor taking the left and top, which moves the source origin with it.
    const Case cases[]{{0,0,0,0,0,12,12,0x00100010},{0x000f0f0f,1,2,3,1,9,7,0x00100010},{0x00030a07,2,1,4,3,8,9,0x00100010},
        {0x00080108,0,0,2,2,10,10,0x000c000e},{0x000f0005,3,3,5,6,6,5,0x00100010}};
    for (const auto& c : cases)
        for (bool direct : {true,false}) {
            Imx1GraphicsCommands::directFills = direct;
            Fixture f; blendPlanes(f);
            // Scissor from 3,4 for the fourth case.
            auto batch = blendBatch(c.alpha,c.sx,c.sy,c.x,c.y,c.w,c.h,c.scissor);
            const bool clipLeftTop = c.scissor == 0x000c000e;
            if (clipLeftTop) batch[19] = 0x00040003;
            const unsigned left = clipLeftTop ? 3 : 0, top = clipLeftTop ? 4 : 0;
            const unsigned right = c.scissor & 0xffff, bottom = c.scissor >> 16;
            assert(f.append(batch,0x200a9788) == A::ok);
            unsigned drawn = 0;
            for (unsigned y = c.y; y < c.y+c.h; ++y) for (unsigned x = c.x; x < c.x+c.w; ++x)
                drawn += x >= left && x < right && y >= top && y < bottom;
            assert(f.commands.duration() == batch.size()+1+drawn);
            f.finish();
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 16; ++x) {
                    const bool inside = x >= c.x && x < c.x+c.w && y >= c.y && y < c.y+c.h &&
                                        x >= left && x < right && y >= top && y < bottom;
                    const uint16_t d = blendDestinationPixel(x,y);
                    const uint16_t want = inside ? blendExpected(d,blendSourcePixel(c.sx+x-c.x,c.sy+y-c.y),c.alpha) : d;
                    if (f.read(blendDestination+(y*16+x)*2) != want) {
                        std::fprintf(stderr,"blend %05x direct %d at %u,%u: %04x, not %04x\n",c.alpha,direct,x,y,
                                     f.read(blendDestination+(y*16+x)*2),want);
                        std::abort();
                    }
                }
        }
    Imx1GraphicsCommands::directFills = true;
    // The host GPU never takes it, so Fast draws it on the CPU as Exact does.
    {
        Fixture f; blendPlanes(f);
        Imx1GraphicsCommands::Fill fill{};
        fill.base = blendDestination; fill.pitch = 32; fill.width = 4; fill.height = 4; fill.a4 = true;
        fill.colour = 0x00030a07; fill.sourceBase = blendSource; fill.sourcePitch = 24;
        Imx1GpuFill drawn;
        assert(!Imx1GraphicsCommands::gpuFill(f.state,f.eim,f.memory,fill,drawn));
    }
    // Refused, with nothing queued: another packet, a second packet, other state, 420/421 not the rectangle,
    // source 1 not source 2, source 2 past its pitch, alpha bits outside the four per channel, and a source that
    // overlaps the destination.
    Fixture f; blendPlanes(f);
    auto refuse = [&](std::vector<uint32_t> batch, const char* what, uint32_t value, uint32_t pc = 0x200a9788) {
        assert(f.append(batch,pc) == A::sideEffect && refused(f,what,value));
        assert(f.read(blendDestination) == blendDestinationPixel(0,0) && f.state.engineState().emptyRing());
    };
    auto good = blendBatch(0x00030a07,0,0,0,0,4,4);
    auto bad = good; bad[30] = 0xc0031a00; bad.insert(bad.end(),{0,0x00010001});
    refuse(bad,"blend-packet",0xc0031a00);
    bad = good; bad.insert(bad.end(),{0xc0011a00,0x00050005,0x00010001});
    refuse(bad,"blend-batch",0xc0011a00);
    bad = good; bad[7] = 0x00080004; refuse(bad,"blend-state",0xd798d4df);
    bad = good; bad[3] = 0x01f01300; refuse(bad,"blend-state",0xd798d4df);
    bad = good; bad[9] = 0x00100f0f; refuse(bad,"blend-state",0xd798d4df);
    bad = good; bad[26] = 5; refuse(bad,"blend-words",0);
    bad = good; bad[15] = blendSource+24; refuse(bad,"blend-words",0);
    bad = good; bad[29] = 9 << 16; refuse(bad,"blend-geometry",9 << 16);
    bad = good; bad[11] = blendSource; bad[13] = 12;
    refuse(bad,"blend-overlap",0);
    // The paint packet from the fill producer still needs the fill state.
    assert(f.append(good,0x200b0200) == A::sideEffect);
    std::puts("PASS: constant blend from 200a9788, alpha per channel, source origin and scissor, refusals");
}
// A queued blend saved mid-batch loads and finishes as a run that never saved.
void blendSaved() {
    auto build = [] {
        return blendBatch(0x00030a07,1,1,2,2,10,9);
    };
    Fixture reference; blendPlanes(reference);
    assert(reference.append(build(),0x200a9788) == A::ok);
    reference.finish();
    Fixture saved; blendPlanes(saved);
    assert(saved.append(build(),0x200a9788) == A::ok);
    std::vector<uint8_t> bytes[2];
    StateWriter configurationOut(bytes[0]); saved.state.state(configurationOut);
    StateWriter commandsOut(bytes[1]); saved.commands.state(commandsOut);
    Fixture loaded; blendPlanes(loaded);
    StateReader configurationIn(bytes[0].data(),bytes[0].size()); loaded.state.state(configurationIn);
    StateReader commandsIn(bytes[1].data(),bytes[1].size()); loaded.commands.state(commandsIn);
    assert(configurationIn.complete() && commandsIn.complete());
    loaded.finish();
    for (unsigned i = 0; i < 16*16; ++i) assert(loaded.read(blendDestination+i*2) == reference.read(blendDestination+i*2));
    assert(loaded.read(blendDestination+(3*16+3)*2) != blendDestinationPixel(3,3));
    std::puts("PASS: a queued constant blend saves, loads and finishes the same");
}
// TwGfxTransformBlt's batch as 200aa564 sends it: the copy state with DP_CNTL giving the directions, then 464
// (source x, y), 465 (where the first source pixel goes) and 466 (destination width, height).
constexpr uint32_t turnDestination = 0xe0a000, turnSource = 0xe0c000;
std::vector<uint32_t> transformBatch(uint32_t control, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                                     uint32_t scissorFrom = 0, uint32_t scissorTo = 0x00100010) {
    return {0x4b2,0x80cc0a00,0x41b,0x5598b4df,0x4b1,0x40030d04,0x472,control,
        0x401,turnDestination,0x402,16,0x46b,turnSource,0x46c,12,0x46f,scissorFrom,0x470,scissorTo,
        0x00020464,sx << 16 | sy,x0 << 16 | y0,w << 16 | h};
}
uint16_t turnPixel(unsigned x, unsigned y) { return uint16_t(0x0821 * x + 0x3100 * y + 7); }
void turnPlanes(Fixture& f) {
    for (unsigned i = 0; i < 16*16; ++i) f.write(turnDestination+i*2,0xa55a);
    for (unsigned y = 0; y < 12; ++y) for (unsigned x = 0; x < 12; ++x) f.write(turnSource+(y*12+x)*2,turnPixel(x,y));
}
void transform() {
    // The eight values the ROM sends (200aa6d8-200aa720 and the plain 3f), each from a 5 by 3 source at 2,1.
    struct Clip { uint32_t from, to; };
    for (uint32_t control : {0x3fu,0x2eu,0x3cu,0x2du,0x3eu,0x3du,0x2fu,0x2cu})
        for (Clip clip : {Clip{0,0x00100010},Clip{0x00050006,0x0009000a}})
            for (bool direct : {true,false}) {
                Imx1GraphicsCommands::directFills = direct;
                const bool majorX = control & 0x10, right = control & 1, down = control & 2;
                const unsigned sw = 5, sh = 3, w = majorX ? sw : sh, h = majorX ? sh : sw;
                const unsigned x0 = right ? 4 : 4 + w - 1, y0 = down ? 6 : 6 + h - 1;
                const unsigned left = clip.from & 0xffff, top = clip.from >> 16, end = clip.to & 0xffff, bottom = clip.to >> 16;
                Fixture f; turnPlanes(f);
                auto batch = transformBatch(control,2,1,x0,y0,w,h,clip.from,clip.to);
                if (f.append(batch,0x200aa8dc) != A::ok) {
                    std::fprintf(stderr,"transform %02x: %d %s %x\n",control,int(f.append(batch,0x200aa8dc)),f.commands.refusal().what ? f.commands.refusal().what : "-",
                                 f.commands.refusal().value);
                    std::abort();
                }
                std::vector<uint16_t> want(16*16,0xa55a);
                unsigned drawn = 0;
                for (unsigned j = 0; j < sh; ++j)
                    for (unsigned i = 0; i < sw; ++i) {
                        const int dx = right ? 1 : -1, dy = down ? 1 : -1;
                        const unsigned x = majorX ? x0 + dx*int(i) : x0 + dx*int(j), y = majorX ? y0 + dy*int(j) : y0 + dy*int(i);
                        if (x < left || x >= end || y < top || y >= bottom) continue;
                        want[y*16+x] = turnPixel(2+i,1+j);
                        ++drawn;
                    }
                assert(f.commands.duration() == batch.size()+1+drawn);
                f.finish();
                for (unsigned i = 0; i < 16*16; ++i)
                    if (f.read(turnDestination+i*2) != want[i]) {
                        std::fprintf(stderr,"transform %02x direct %d clip %08x at %u,%u: %04x, not %04x\n",control,direct,
                                     clip.from,i%16,i/16,f.read(turnDestination+i*2),want[i]);
                        std::abort();
                    }
            }
    Imx1GraphicsCommands::directFills = true;
    // Rotated by 180 degrees: the source's top left lands at the bottom right.
    {
        Fixture f; turnPlanes(f);
        assert(f.append(transformBatch(0x3c,0,0,9,9,4,4),0x200aa8dc) == A::ok);
        f.finish();
        assert(f.read(turnDestination+(9*16+9)*2) == turnPixel(0,0) && f.read(turnDestination+(6*16+6)*2) == turnPixel(3,3) &&
               f.read(turnDestination+(9*16+6)*2) == turnPixel(3,0));
    }
    // A destination past the surface's right edge is drawn where the scissor trims it to the surface (the ROM
    // checks only the source against its surface), and refused where the scissor leaves part of it outside.
    {
        Fixture f; turnPlanes(f);
        assert(f.append(transformBatch(0x3f,0,0,14,3,5,3),0x200aa8dc) == A::ok);
        f.finish();
        for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x)
            assert(f.read(turnDestination+(y*16+x)*2) == (x >= 14 && y >= 3 && y < 6 ? turnPixel(x-14,y-3) : 0xa55a));
        Fixture g; turnPlanes(g);
        assert(g.append(transformBatch(0x3f,0,0,14,3,5,3,0,0x00100014),0x200aa8dc) == A::sideEffect &&
               refused(g,"transform-geometry",5 << 16 | 3) && g.state.engineState().emptyRing());
    }
    // The host GPU never takes it, so Fast draws it on the CPU as Exact does.
    {
        Fixture f; turnPlanes(f);
        assert(f.append(transformBatch(0x2e,0,0,2,2,3,5),0x200aa8dc) == A::ok);
        Imx1GpuFill drawn;
        Imx1GraphicsCommands::Fill fill{};
        fill.base = turnDestination; fill.pitch = 32; fill.width = 2; fill.height = 2; fill.plain = fill.keyEqual = true;
        fill.sourceBase = turnSource+30; fill.sourcePitch = 2; fill.colour = uint32_t(-24);
        assert(!Imx1GraphicsCommands::gpuFill(f.state,f.eim,f.memory,fill,drawn));
        f.finish();
    }
    // Refused, with nothing queued: other GMC, mix, datatype, direction or 48f, a corner that runs past 0, the source past
    // its pitch, a source that overlaps the destination, and a second blit in one batch.
    Fixture f; turnPlanes(f);
    auto refuse = [&](std::vector<uint32_t> batch, const char* what, uint32_t value, uint32_t pc = 0x200aa8dc) {
        assert(f.append(batch,pc) == A::sideEffect && refused(f,what,value));
        assert(f.read(turnDestination) == 0xa55a && f.state.engineState().emptyRing());
    };
    const auto good = transformBatch(0x2e,0,0,6,2,3,5);
    auto bad = good; bad[3] = 0x55e0b4df; refuse(bad,"transform-state",0x55e0b4df);
    bad = good; bad[1] = 0x00cc0a00; refuse(bad,"transform-state",0x2e);
    bad = good; bad[5] = 0x40020d04; refuse(bad,"transform-state",0x2e);
    bad = good; bad[7] = 0x2b; refuse(bad,"transform-state",0x2b);
    bad = good; bad.insert(bad.begin(),{0x48f,0}); refuse(bad,"transform-state",0x2e);
    bad = good; bad[22] = 1 << 16 | 2; refuse(bad,"transform-geometry",good[23]);
    bad = good; bad[21] = 10 << 16; refuse(bad,"transform-geometry",good[23]);
    bad = good; bad[9] = turnSource; bad[11] = 12; refuse(bad,"transform-overlap",good[22]);
    bad = good; bad.insert(bad.end(),{0x00020464,0,0x00060002,0x00030005}); refuse(bad,"transform-batch",0x464);
    // Its words from any other producer are refused, and so is a paint packet from it.
    assert(f.append(good,0x200a9ed4) == A::sideEffect);
    bad = good; bad.resize(20); bad.insert(bad.end(),{0xc0011a00,0,0x00010001});
    assert(f.append(bad,0x200aa8dc) != A::ok);
    std::puts("PASS: rotate and mirror blit from 200aa8dc, all eight directions, scissor, refusals");
}
// A queued rotate and mirror blit saved mid-batch loads and finishes as a run that never saved.
void transformSaved() {
    Fixture reference; turnPlanes(reference);
    assert(reference.append(transformBatch(0x2d,1,2,8,8,4,6,0x00030002,0x0009000a),0x200aa8dc) == A::ok);
    reference.finish();
    Fixture saved; turnPlanes(saved);
    assert(saved.append(transformBatch(0x2d,1,2,8,8,4,6,0x00030002,0x0009000a),0x200aa8dc) == A::ok);
    std::vector<uint8_t> bytes[2];
    StateWriter configurationOut(bytes[0]); saved.state.state(configurationOut);
    StateWriter commandsOut(bytes[1]); saved.commands.state(commandsOut);
    Fixture loaded; turnPlanes(loaded);
    StateReader configurationIn(bytes[0].data(),bytes[0].size()); loaded.state.state(configurationIn);
    StateReader commandsIn(bytes[1].data(),bytes[1].size()); loaded.commands.state(commandsIn);
    assert(configurationIn.complete() && commandsIn.complete());
    loaded.finish();
    unsigned changed = 0;
    for (unsigned i = 0; i < 16*16; ++i) {
        assert(loaded.read(turnDestination+i*2) == reference.read(turnDestination+i*2));
        changed += loaded.read(turnDestination+i*2) != 0xa55a;
    }
    assert(changed == 12);
    std::puts("PASS: a queued rotate and mirror blit saves, loads and finishes the same");
}
int main() {
    hostSourceType();
    geometry(); guards(); scissor(); stretchBlit(); keyedModes(); brushUpload(); fills(); stretch(); sideBySide(); scanStatus(); executionAndReset(); privateCapture();
    stretchStaleGmc(); stretchRatios(); lines(); linesSaved(); clippedPastPitch(); blend(); blendSaved(); transform(); transformSaved();
    std::puts("PASS: plain RGB565 copy geometry, bounds, atomic append, queued A4 execution-time source, deadlines, reset and routes");
}
