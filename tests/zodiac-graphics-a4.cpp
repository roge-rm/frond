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
    A append(const std::vector<uint32_t>& batch, uint32_t pc = 0x200a9a9c) {
        const auto begin = state.engineState().producer();
        for (unsigned i = 0; i < batch.size(); ++i) write(0x150000+((begin+i)&16383)*4,batch[i],4);
        auto candidate = state;
        auto work = commands;
        uint32_t end = (begin+uint32_t(batch.size()))&16383;
        auto status = candidate.access(eim,pc,0x16010220,4,true,end);
        if (status == A::ok) status = work.prepare(candidate,eim,memory);
        if (status == A::ok) {
            candidate.startEngine(work.duration());
            state = candidate;
            commands = work;
        }
        return status;
    }
    void finish() {
        assert(state.advanceCpuCycles(uint32_t(commands.duration())));
        assert(commands.complete(state,eim,memory) == A::ok);
    }
};
constexpr uint32_t destination = 0xe01000, source = 0xe03000;
std::vector<uint32_t> packet(uint32_t width, uint32_t height, uint32_t dp = 20, uint32_t sp = 24,
                             uint32_t xy = 0) {
    std::vector<uint32_t> out{0x41b,0xd798d4df,0x4b2,0x01cc1300,0x4b1,0x40057d04,
        0x488,0x80004,0x401,destination,0x402,dp,0x46b,source,0x46c,sp,
        0x418,source,0x419,sp,0x420,width,0x421,height,0x416,0,0x46f,0,
        0x470,((height+(xy>>16))<<16)|(width+(xy&0xffff))};
    const uint32_t count = (width*height+7)/8;
    out.insert(out.end(),{0xc0001900u|((count+1)<<16),xy,(height<<16)|width});
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t word = 0;
        for (unsigned n = 0; n < 8; ++n) word |= ((i*8+n)%16) << (n*4);
        out.push_back(word);
    }
    return out;
}
// Independent integer oracle for the declared virtual rule, using repeated additions/division.
uint16_t expected(uint16_t d, uint16_t s, unsigned alpha) {
    uint16_t result = 0;
    for (unsigned shift : {0u,5u,11u}) {
        const unsigned mask = shift == 5 ? 63 : 31;
        unsigned total = 0, channel = 0;
        for (unsigned i = 0; i < 15; ++i) total += ((i < alpha ? s : d) >> shift)&mask;
        while (total >= 15) { ++channel; total -= 15; }
        result |= channel << shift;
    }
    return result;
}
void planes(Fixture& f, uint32_t w, uint32_t h, uint32_t dp = 20, uint32_t sp = 24,
            uint16_t d = 0x92cb, uint16_t s = 0x2d95) {
    for (uint32_t y = 0; y < h+2; ++y)
        for (uint32_t x = 0; x < dp; ++x) f.write(destination+(y*dp+x)*2,d);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < sp; ++x) f.write(source+(y*sp+x)*2,s);
    assert(w <= dp && w <= sp);
}
void replace(std::vector<uint32_t>& p, uint32_t reg, uint32_t value) {
    for (size_t i = 0; i < 30; i += 2)
        if (p[i] == reg) { p[i+1] = value; return; }
    p.insert(p.begin()+30,{reg,value});
}
std::vector<uint8_t> bytes(const char* path) {
    std::ifstream file(path,std::ios::binary); assert(file);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),{});
}
void privateCapture() {
    const char* p = std::getenv("FROND_ZODIAC_A4_PACKET");
    const char* m = std::getenv("FROND_ZODIAC_A4_MASK");
    const char* d = std::getenv("FROND_ZODIAC_A4_DESTINATION");
    const char* s = std::getenv("FROND_ZODIAC_A4_SOURCE2");
    assert((p && m && d && s) || (!p && !m && !d && !s));
    if (!p) return;
    const auto captured = bytes(p), mask = bytes(m), dst = bytes(d), src = bytes(s);
    assert(captured.size() == 272 && mask.size() == 208 && dst.size() == 832 && src.size() == 832);
    std::vector<uint32_t> words;
    for (size_t i = 0; i < captured.size(); i += 4)
        words.push_back(uint32_t(captured[i])|uint32_t(captured[i+1])<<8|uint32_t(captured[i+2])<<16|uint32_t(captured[i+3])<<24);
    Fixture f;
    // Retained context at the actual submission, followed by the unchanged 68-word packet.
    assert(f.append({0x401,0xf50980,0x402,16,0x46f,0,0x470,0x001a000e},0x200b0200) == A::ok);
    f.finish();
    for (size_t i = 0; i < 832; ++i) { f.write(0xf50980+i,dst[i],1); f.write(0xf50cc0+i,src[i],1); }
    assert(f.append(words) == A::ok && f.commands.duration() == 433);
    f.finish();
    std::ofstream results;
    if (const char* output = std::getenv("FROND_ZODIAC_A4_RESULTS")) {
        results.open(output); assert(results);
        results << "x,y,alpha,destination,source2,virtual_expected,actual\n";
    }
    for (unsigned y = 0; y < 26; ++y) {
        for (unsigned x = 0; x < 14; ++x) {
            const size_t at = y*32+x*2;
            const unsigned alpha = (mask[y*8+x/2] >> (x%2 ? 0 : 4))&15;
            const auto before = uint16_t(dst[at]|dst[at+1]<<8), colour = uint16_t(src[at]|src[at+1]<<8);
            const auto want = expected(before,colour,alpha), actual = uint16_t(f.read(0xf50980+at));
            assert(actual == want);
            if (results.is_open()) results << x << ',' << y << ',' << alpha << ',' << before << ',' << colour << ',' << want << ',' << actual << '\n';
        }
        for (unsigned x = 28; x < 32; ++x) assert(f.read(0xf50980+y*32+x,1) == dst[y*32+x]);
    }
    std::puts("PASS: all 364 private capture pixels match the virtual policy and independent ROM mask/real RGB565 planes");
}
}
int main() {
    assert(!Imx1Cs5Configuration().a4SourceOverEnabled());
    assert(!Imx1Cs5Configuration(Imx1GraphicsProfile::strict,true).a4SourceOverEnabled());
    Fixture disabled(false);
    assert(disabled.append(packet(8,2)) != A::ok);
    for (auto colour : {std::pair<uint16_t,uint16_t>{0x92cb,0x2d95},{0xffff,0x2b75},
                        {0x001f,0xf800},{0x39c7,0x39c7},{0x07e0,0xf81f}}) {
        Fixture f;
        planes(f,8,2,20,24,colour.first,colour.second);
        const auto p = packet(8,2);
        const auto begin = f.state.engineState().consumer();
        const auto irq = f.state.interruptState().pendingValue();
        assert(f.append(p) == A::ok && f.commands.duration() == p.size()+17);
        assert(f.read(destination) == colour.first && f.state.engineState().consumer() == begin);
        assert(f.state.advanceCpuCycles(uint32_t(f.commands.duration()-1)));
        assert(f.commands.complete(f.state,f.eim,f.memory) == A::sequence);
        assert(f.state.advanceCpuCycles(1) && f.commands.complete(f.state,f.eim,f.memory) == A::ok);
        assert(f.state.engineState().emptyRing() && f.state.interruptState().pendingValue() == irq);
        for (unsigned i = 0; i < 16; ++i) assert(f.read(destination+((i/8)*20+i%8)*2) == expected(colour.first,colour.second,i));
        assert(f.read(destination+16) == colour.first);
    }
    // The text producer's foreground and background in 490 and 491 don't change the result.
    {
        Fixture f;
        planes(f,8,2,20,24,0x92cb,0x2d95);
        auto p = packet(8,2);
        replace(p,0x490,0); replace(p,0x491,0xffff);
        assert(f.append(p) == A::ok);
        f.finish();
        for (unsigned i = 0; i < 16; ++i) assert(f.read(destination+((i/8)*20+i%8)*2) == expected(0x92cb,0x2d95,i));
    }
    // An odd height: 6x3 is 18 samples, so the third word is a quarter used and its other samples are dropped.
    {
        Fixture f;
        planes(f,6,3,20,24,0x92cb,0x2d95);
        const auto p = packet(6,3);
        assert((p[p.size()-6] >> 16) == 0xc004 && f.append(p) == A::ok);
        f.finish();
        for (unsigned i = 0; i < 18; ++i) assert(f.read(destination+((i/6)*20+i%6)*2) == expected(0x92cb,0x2d95,i%16));
        assert(f.read(destination+(3*20)*2) == 0x92cb && f.read(destination+(2*20+6)*2) == 0x92cb);
        // An odd width stays refused.
        Fixture g;
        planes(g,6,3,20,24,0x92cb,0x2d95);
        assert(g.append(packet(5,3)) != A::ok);
    }
    // Different row strides, nonzero destination origin, lower-edge clipping and spatial colour planes.
    {
        Fixture f;
        planes(f,6,4);
        auto p = packet(6,4,20,24,0x00010002);
        replace(p,0x46f,0x00020003);
        assert(f.append(p) == A::ok && f.commands.duration() == p.size()+1+15);
        // Source and destination changes made after preflight are read during execution.
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 6; ++x) {
                f.write(source+(y*24+x)*2,0x1234+x*97+y*301);
                f.write(destination+((y+1)*20+x+2)*2,0xbcdf-x*17-y*29);
            }
        f.finish();
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 6; ++x) {
                const auto d = uint16_t(0xbcdf-x*17-y*29), s = uint16_t(0x1234+x*97+y*301);
                assert(f.read(destination+((y+1)*20+x+2)*2) == (!x || !y ? d : expected(d,s,(y*6+x)%16)));
            }
    }
    {
        Fixture f; planes(f,2,2);
        auto p = packet(2,2); replace(p,0x46f,0x00040004);
        assert(f.append(p) == A::ok && f.commands.duration() == p.size()+1);
        f.finish();
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 2; ++x) assert(f.read(destination+(y*20+x)*2) == 0x92cb);
    }
    // The final four padding nibbles are owned and consumed without painting a pixel.
    {
        Fixture f; planes(f,6,2);
        auto p = packet(6,2); p.back() |= 0xffff0000;
        assert(f.append(p) == A::ok);
        const auto begin = f.state.engineState().consumer();
        for (unsigned i = 0; i < p.size(); ++i) f.write(0x150000+((begin+i)&16383)*4,0,4);
        auto owned = f.commands; f.commands.reset(); f.commands = owned;
        f.finish();
        for (unsigned i = 0; i < 12; ++i) assert(f.read(destination+(i/6*20+i%6)*2) == expected(0x92cb,0x2d95,i));
        assert(f.read(destination+12) == 0x92cb);
    }
    // Reject complete malformed/state variants atomically, including with earlier work pending.
    {
        Fixture f; planes(f,8,2);
        const std::vector<uint32_t> solid{0x401,destination,0x402,20,0x41f,0x92cb,0xc0011a00,0,0x00080002};
        assert(f.append(solid,0x200b0200) == A::ok);
        const auto oldTicks = f.commands.duration();
        const auto oldEnd = f.state.engineState().producer();
        auto reject = [&](const std::vector<uint32_t>& bad, uint32_t pc = 0x200a9a9c) {
            const auto generation = f.memory.dirty(1).generation;
            const auto irq = f.state.interruptState().pendingValue();
            const auto clock = f.state.clockState().count();
            assert(f.append(bad,pc) != A::ok);
            assert(f.commands.duration() == oldTicks && f.state.engineState().producer() == oldEnd);
            assert(f.memory.dirty(1).generation == generation && f.state.interruptState().pendingValue() == irq);
            assert(f.state.clockState().count() == clock);
        };
        const auto valid = packet(8,2);
        for (auto change : {std::pair<uint32_t,uint32_t>{0x41b,0xd798d4de},{0x4b2,0x01cc0b00},
            {0x4b1,0x40057c04},{0x4b1,0x40056d04},{0x4b1,0x40055d04},{0x4b1,0x40037d04},{0x488,0x01080004},{0x488,4},{0x484,0xe0f0f},
            {0x48f,0xfffe},{0x4b3,0xffff},{0x472,0x3e},{0x471,0x1fff0007},
            {0x416,1},{0x416,0xffff0000},{0x420,7},{0x421,3},{0x419,0},{0x419,8193},
            {0x418,source+1},{0x418,0xfffffe},{0x46b,source+2},{0x46c,25},{0x401,destination+1},
            {0x401,0xfffffffe},{0x402,0},{0x402,8193},{0x402,7},{0x418,destination},
            {0x4a1,1},{0x4a0,1},{0x42e,1},{0x41d,1},{0x30f,0},{0x470,0x00010004}}) {
            auto bad = valid; replace(bad,change.first,change.second); reject(bad);
        }
        // Exercise bounds and overlap after both copies of source state agree.
        for (uint32_t address : {0u,0xfffffeu,0xfffffffeu,source+1,destination,destination+8}) {
            auto bad = valid; replace(bad,0x418,address); replace(bad,0x46b,address); reject(bad);
        }
        for (uint32_t pitch : {0u,7u,8193u,0xffffffffu}) {
            auto bad = valid; replace(bad,0x419,pitch); replace(bad,0x46c,pitch); reject(bad);
        }
        for (uint32_t reg : {0x418u,0x419u,0x420u,0x421u,0x416u}) {
            auto bad = valid;
            for (size_t i = 0; i < 30; i += 2)
                if (bad[i] == reg) { bad.erase(bad.begin()+i,bad.begin()+i+2); break; }
            reject(bad);
        }
        for (auto wh : {0u,0x00020007u,0x00030008u,0x00000008u,0xffffffffu,0x20000008u}) {
            auto bad = valid; bad[32] = wh; reject(bad);
        }
        for (uint32_t xy : {0xffffffffu,0xffff0000u,0x0000ffffu}) { auto bad = valid; bad[31] = xy; reject(bad); }
        for (unsigned n = 0; n < valid.size(); ++n) reject({valid.begin(),valid.begin()+n});
        for (unsigned i : {0u,2u,4u,30u}) { auto bad = valid; bad[i] ^= 1; reject(bad); }
        auto bad = valid; bad.push_back(0); reject(bad);
        bad = valid; bad.insert(bad.end(),valid.begin()+30,valid.end()); reject(bad);
        for (uint32_t pc : {0u,0x200a9a98u,0x200a9aa0u,0x200ac300u,0x200b0200u}) reject(valid,pc);
        // Explicit MIX after GMC wins; GMC cannot invent/reset the source2 selector.
        bad = valid; bad.insert(bad.begin()+30,{0x4b2,0x01cc0b00,0x41b,0xd798d4df}); reject(bad);
        assert(f.append(valid) == A::ok);
        assert(f.state.advanceCpuCycles(uint32_t(oldTicks+f.commands.duration())));
        assert(f.commands.complete(f.state,f.eim,f.memory) == A::ok && f.state.engineState().emptyRing());
        for (unsigned i = 0; i < 16; ++i) assert(f.read(destination+(i/8*20+i%8)*2) == expected(0x92cb,0x2d95,i));
    }
    // Upload source2, keyed-copy destination, then A4, with copied command ownership and one combined advance.
    {
        Fixture f; planes(f,8,2,20,24,0x92cb,0x2d95);
        const uint32_t staging = 0xe05000;
        std::vector<uint32_t> host{0x41b,0x5798b4df,0x4b2,0x00cc0b00,0x4b1,0x40030d04,
            0x401,source,0x402,24,0x470,0x00020008,0xc0091900,0,0x00020008};
        for (unsigned i = 0; i < 8; ++i) host.push_back(0xf800f800);
        assert(f.append(host,0x200ac300) == A::ok);
        const auto hostTicks = f.commands.duration();
        const auto hostEnd = f.state.engineState().producer();
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 8; ++x) f.write(staging+(y*20+x)*2,0x001f);
        const std::vector<uint32_t> copy{0x41b,0x4598b4df,0x4b2,0x00cc0a00,0x401,destination,0x402,20,
            0x46b,staging,0x46c,20,0xc0051c00,0x01000005,0xf800,0,0,0,0x00080002};
        assert(f.append(copy,0x200aa504) == A::ok);
        const auto copyTicks = f.commands.duration();
        const auto copyEnd = f.state.engineState().producer();
        auto p = packet(8,2); // inherited arithmetic, DT and MIX are reset explicitly in the packet
        assert(f.append(p) == A::ok);
        const auto a4Ticks = f.commands.duration();
        auto owned = f.commands; f.commands.reset(); f.commands = owned;
        assert(f.read(source) == 0x2d95 && f.read(destination) == 0x92cb);
        assert(f.state.advanceCpuCycles(uint32_t(hostTicks)) && f.commands.complete(f.state,f.eim,f.memory) == A::ok);
        assert(f.state.engineState().consumer() == hostEnd && f.read(source) == 0xf800);
        assert(f.state.advanceCpuCycles(uint32_t(copyTicks)) && f.commands.complete(f.state,f.eim,f.memory) == A::ok);
        assert(f.state.engineState().consumer() == copyEnd && f.read(destination) == 0x001f);
        assert(f.state.advanceCpuCycles(uint32_t(a4Ticks)) && f.commands.complete(f.state,f.eim,f.memory) == A::ok);
        for (unsigned i = 0; i < 16; ++i) assert(f.read(destination+(i/8*20+i%8)*2) == expected(0x001f,0xf800,i));
        // Return through host, memory and solid with explicit proven state resets.
        host.insert(host.begin(),{0x488,4});
        assert(f.append(host,0x200ac300) == A::ok); f.finish();
        assert(f.append(copy,0x200aa504) == A::ok); f.finish();
        const std::vector<uint32_t> solid{0x41b,0x55e0b4df,0x4b2,0x00f00a00,0x41f,0x1234,0xc0011a00,0,0x00080002};
        assert(f.append(solid,0x200b0200) == A::ok); f.finish();
        assert(f.read(destination) == 0x1234);
        assert(f.append(p) == A::ok);
        const auto configured = f.state;
        f.state.reset(); f.commands.reset();
        assert(f.commands.complete(f.state,f.eim,f.memory) == A::sequence);
        uint32_t retained = 0;
        assert(f.memory.accessGpu(configured,f.eim,destination,2,false,retained) == A::ok && retained == 0x1234);
        assert(f.state.a4SourceOverEnabled());
    }
    privateCapture();
    std::puts("PASS: opt-in virtual A4 source-over RGB565, all alpha levels, geometry/ownership/queue/state guards");
}
