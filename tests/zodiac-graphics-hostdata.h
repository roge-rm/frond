#ifndef FROND_TEST_ZODIAC_GRAPHICS_HOSTDATA_H
#define FROND_TEST_ZODIAC_GRAPHICS_HOSTDATA_H

#include <cstdlib>
#include <fstream>
#include <iterator>

inline void checkHostData(const Imx1Cs5Configuration& boot, const Imx1GraphicsCommands& bootCommands,
                          const Imx1Eim& eim) {
    using A = Imx1BootstrapAccess;
    auto state = boot;
    auto commands = bootCommands;
    Imx1GraphicsMemory memory;
    auto put = [&](const Imx1Cs5Configuration& config, uint32_t index, uint32_t word) {
        assert(memory.accessGpu(config,eim,0x150000+(index & 16383)*4,4,true,word) == A::ok);
    };
    // Advance the completed boot ring from word 47 to the captured consumer at 64.
    std::vector<uint32_t> bridge;
    for (int i = 0; i < 5; ++i)
        for (uint32_t w : {0x4023841fu,0xffffu,0x014001e0u}) bridge.push_back(w);
    bridge.push_back(0x4a0); bridge.push_back(0);
    for (unsigned i = 0; i < bridge.size(); ++i) put(state,47+i,bridge[i]);
    uint32_t value = 64;
    assert(state.access(eim,0x200b0200,0x16010220,4,true,value) == A::ok);
    assert(commands.prepare(state,eim,memory) == A::ok);
    state.startEngine(commands.duration());
    assert(state.advanceCpuCycles(uint32_t(commands.duration())));
    assert(commands.complete(state,eim,memory) == A::ok && state.engineState().consumer() == 64);
    const auto ready = state;
    const auto readyCommands = commands;
    const std::vector<uint32_t> capturedState{0x4025941b,0x5798b4df,0x00cc0b00,
        0x40200c70,0x0022002c,0x00f51000,0x402,0x140};
    auto packet = [&](uint32_t width, uint32_t height, uint32_t xy = 0) {
        auto batch = capturedState;
        batch.push_back(0xc0001900u | ((width/2*height+1) << 16));
        batch.push_back(xy); batch.push_back(height << 16 | width);
        for (uint32_t i = 0; i < width/2*height; ++i) batch.push_back(0xf800f800);
        return batch;
    };
    auto load = [&](const std::vector<uint32_t>& batch) {
        for (unsigned i = 0; i < batch.size(); ++i) put(ready,64+i,batch[i]);
    };
    auto prepare = [&](const std::vector<uint32_t>& batch, Imx1Cs5Configuration& candidate,
                       Imx1GraphicsCommands& work, uint32_t pc = 0x200ac300) {
        load(batch);
        uint32_t producer = (64+uint32_t(batch.size())) & 16383;
        const auto status = candidate.access(eim,pc,0x16010220,4,true,producer);
        return status == A::ok ? work.prepare(candidate,eim,memory) : status;
    };
    auto read = [&](uint32_t address, uint32_t size = 2) {
        uint32_t v = 0;
        assert(memory.accessGpu(ready,eim,address,size,false,v) == A::ok);
        return v;
    };
    auto run = [&](std::vector<uint32_t> batch, uint64_t pixels, bool overwriteRing = false) {
        auto candidate = ready;
        auto work = readyCommands;
        assert(prepare(batch,candidate,work) == A::ok);
        const auto pending = candidate.interruptState().pendingValue();
        const auto count = candidate.clockState().count();
        assert(work.duration() == batch.size()+1+pixels);
        assert(read(0xf51000) == 0);
        assert(candidate.engineState().consumer() == 64);
        assert(candidate.engineState().hostDataSubmission());
        uint32_t producer = (64+uint32_t(batch.size())) & 16383;
        assert(candidate.access(eim,0x200ac300,0x16010220,4,true,producer) == A::sequence);
        assert(work.prepare(candidate,eim,memory) == A::sequence);
        assert(candidate.interruptState().pendingValue() == pending && candidate.clockState().count() == count);
        if (overwriteRing) for (unsigned i = 0; i < batch.size(); ++i) put(ready,64+i,0);
        // Copying a pending command also owns all pixels independently.
        auto copied = work;
        work.reset();
        candidate.startEngine(copied.duration());
        assert(candidate.advanceCpuCycles(uint32_t(copied.duration()-1)));
        assert(!candidate.engineState().completed() && candidate.engineState().consumer() == 64);
        assert(copied.complete(candidate,eim,memory) == A::sequence);
        assert(read(0xf51000) == 0);
        value = 0;
        assert(candidate.access(eim,0x200b8cb0,0x16010140,4,false,value) == A::ok && value == 0x80000000);
        assert(candidate.access(eim,0x200b8c64,0x1601021c,4,false,value) == A::ok && value == 64);
        assert(candidate.access(eim,0x200b8c70,0x16010220,4,false,value) == A::ok && value == producer);
        assert(candidate.advanceCpuCycles(1) && candidate.engineState().completed());
        const auto irqBeforeComplete = candidate.interruptState().pendingValue();
        assert(copied.complete(candidate,eim,memory) == A::ok);
        assert(candidate.engineState().consumer() == producer && candidate.engineState().emptyRing());
        assert(candidate.interruptState().pendingValue() == irqBeforeComplete);
        assert(copied.complete(candidate,eim,memory) == A::sequence);
        assert(candidate.access(eim,0x200b8cb0,0x16010140,4,false,value) == A::ok && value == 0);
    };
    auto clearSurface = [&]() {
        for (uint32_t i = 0; i < 34*640; i += 4) {
            uint32_t v = 0;
            assert(memory.accessGpu(ready,eim,0xf51000+i,4,true,v) == A::ok);
        }
    };
    auto red = packet(44,34);
    assert(red.size() == 759 && 64+red.size() == 0x337 && red[8] == 0xc2ed1900);
    run(red,44*34,true);
    for (uint32_t y = 0; y < 34; ++y) {
        for (uint32_t x = 0; x < 44; ++x) assert(read(0xf51000+y*640+x*2) == 0xf800);
        assert(read(0xf51000+y*640+88) == 0);
    }
    clearSurface();
    const auto packetPath = std::getenv("FROND_ZODIAC_HOSTDATA_PACKET");
    const auto sourcePath = std::getenv("FROND_ZODIAC_HOSTDATA_SOURCE");
    assert(bool(packetPath) == bool(sourcePath));
    if (packetPath) {
        auto bytes = [](const char* path) {
            std::ifstream file(path,std::ios::binary);
            assert(file);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),{});
        };
        const auto capturedBytes = bytes(packetPath), sourceBytes = bytes(sourcePath);
        assert(capturedBytes.size() == 759*4 && sourceBytes.size() == 44*34*2);
        std::vector<uint32_t> captured;
        for (size_t at = 0; at < capturedBytes.size(); at += 4)
            captured.push_back(uint32_t(capturedBytes[at]) | uint32_t(capturedBytes[at+1]) << 8 |
                uint32_t(capturedBytes[at+2]) << 16 | uint32_t(capturedBytes[at+3]) << 24);
        for (unsigned i = 0; i < 11; ++i) assert(captured[i] == red[i]);
        for (unsigned i = 0; i < 748; ++i)
            assert(captured[11+i] == (uint32_t(sourceBytes[i*4]) << 8 | sourceBytes[i*4+1] |
                uint32_t(sourceBytes[i*4+2]) << 24 | uint32_t(sourceBytes[i*4+3]) << 16));
        run(captured,44*34,true);
        for (uint32_t y = 0; y < 34; ++y) {
            for (uint32_t x = 0; x < 44; ++x) {
                const auto at = (y*44+x)*2;
                assert(read(0xf51000+y*640+x*2) == (uint32_t(sourceBytes[at]) << 8 | sourceBytes[at+1]));
            }
            assert(read(0xf51000+y*640+88) == 0);
        }
        clearSurface();
        std::puts("PASS: private captured759-word host-data packet matches all1496 source-derived pixels");
    }
    // Palm big-endian source bytes, two payload words per row, source pitch 12.
    const std::vector<uint8_t> source{0x12,0x34,0xab,0xcd,0x56,0x78,0x9a,0xbc,0xee,0xee,0xee,0xee,
        0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,0xee,0xee,0xee,0xee,
        0xf8,0x00,0x07,0xe0,0x00,0x1f,0xff,0xff,0xee,0xee,0xee,0xee};
    auto colours = packet(4,3,0x00020001); // destination x1,y2, destination pitch 320 pixels
    colours[4] = 0x00050005; // upper bounds beyond the encoded rectangle are unambiguous
    colours.insert(colours.begin()+8,{0x46f,0x00030002}); // clip first source row and column
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 4; x += 2) {
            const auto at = y*12+x*2;
            colours[13+y*2+x/2] = uint32_t(source[at])<<8 | source[at+1] |
                uint32_t(source[at+2])<<24 | uint32_t(source[at+3])<<16;
        }
    run(colours,6,true);
    assert(read(0xf51000+3*640+2*2) == 0x4567 && read(0xf51000+3*640+3*2) == 0x89ab);
    assert(read(0xf51000+4*640+2*2) == 0x07e0 && read(0xf51000+4*640+3*2) == 0x001f);
    assert(read(0xf51000+3*640+4*2) == 0xcdef && read(0xf51000+4*640+4*2) == 0xffff);
    assert(read(0xf51000+3*640+4,1) == 0x67 && read(0xf51000+3*640+5,1) == 0x45);
    for (auto address : {0xf51000+2*640+4,0xf51000+3*640+2,0xf51000+3*640+10,0xf51000+5*640+4})
        assert(read(address) == 0);
    clearSurface();
    for (auto wh : {std::pair<uint32_t,uint32_t>{2,1},{2,3},{18,2}}) {
        run(packet(wh.first,wh.second),wh.first*wh.second);
        clearSurface();
    }
    auto reject = [&](const std::vector<uint32_t>& bad, uint32_t pc = 0x200ac300) {
        auto candidate = ready;
        auto work = readyCommands;
        load(bad);
        const auto internal = memory.dirty(0), external = memory.dirty(1);
        uint32_t producer = (64+uint32_t(bad.size())) & 16383;
        assert(candidate.access(eim,pc,0x16010220,4,true,producer) == A::ok);
        assert(work.prepare(candidate,eim,memory) != A::ok);
        assert(work.duration() == readyCommands.duration());
        assert(memory.dirty(0).generation == internal.generation && memory.dirty(1).generation == external.generation);
        assert(ready.engineState().consumer() == 64 && ready.engineState().producer() == 64);
        assert(!ready.engineState().hostDataSubmission() && !ready.engineState().completed());
        assert(candidate.engineState().consumer() == 64 && candidate.engineState().producer() == producer);
        assert(candidate.interruptState().pendingValue() == ready.interruptState().pendingValue());
        assert(candidate.clockState().count() == ready.clockState().count());
        assert(read(0xf51000) == 0);
        // Discard the rejected MMIO candidate; the same command state can accept either route next.
        candidate = ready;
        const std::vector<uint32_t> solid{0xc0011a00,0,0x00020001};
        assert(prepare(solid,candidate,work,0x200b0200) == A::ok);
        candidate = ready; work = readyCommands;
        assert(prepare(packet(2,1),candidate,work) == A::ok);
    };
    auto small = packet(4,3,0x00020001);
    for (unsigned i : {0u,3u,6u}) { auto bad = small; bad[i] = 0x80000000; reject(bad); }
    for (uint32_t header : {0xc0071800u,0xc0071a00u,0xc0071b00u,0xc0071901u,0xc0061900u,0xc0081900u}) {
        auto bad = small; bad[8] = header; reject(bad);
    }
    for (uint32_t wh : {0u,0x00030003u,0x00030000u,0x00000004u,0xffffffffu,0x20010004u}) {
        auto bad = small; bad[10] = wh; reject(bad);
    }
    for (unsigned index : {1u,2u}) { auto bad = small; bad[index] ^= 1; reject(bad); }
    for (uint32_t base : {0xf51001u,0xfffffeu,0xfffffffeu,0x15fffeu,0u}) {
        auto bad = small; bad[5] = base; reject(bad);
    }
    for (uint32_t pitch : {0u,4u,8193u,0xffffffffu}) { auto bad = small; bad[7] = pitch; reject(bad); }
    for (uint32_t xy : {0xffff0001u,0x0002ffffu,0xffffffffu}) { auto bad = small; bad[9] = xy; reject(bad); }
    for (unsigned n : {1u,2u,3u,8u}) { auto bad = small; bad.resize(bad.size()-n); reject(bad); }
    auto bad = small; bad.push_back(0xf800f800); reject(bad);
    bad = small; bad.insert(bad.end(),small.begin()+8,small.end()); reject(bad);
    bad = small; bad.insert(bad.end(),{0x401,0xffffffff,0xc0011a00,0,0x00020001}); reject(bad);
    // Interior upper edges, including odd visible-width padding, remain unqualified.
    for (uint32_t clip : {0x00050003u,0x00040005u,0x00040003u}) {
        auto clipped = small; clipped[4] = clip; reject(clipped);
    }
    reject(capturedState);
    reject({0xc0011a00,0,0x00020001});
    reject(small,0x200b0200);
    reject({0x401,0xffffffff,0xc0011a00,0,0x00020001},0x200b0200);
    // A new command decoder has no inherited native state.
    auto missing = ready; Imx1GraphicsCommands missingWork;
    assert(prepare(small,missing,missingWork) != A::ok);
    // Unknown producers, bad widths, busy/reset/cancel and strict mode retain their guards.
    for (uint32_t pc : {0x200ac2fcu,0x200ac304u,0u}) {
        auto candidate = ready; value = 80;
        assert(candidate.access(eim,pc,0x16010220,4,true,value) == A::externalInput);
        assert(candidate.engineState().emptyRing() && !candidate.engineState().hostDataSubmission());
    }
    for (uint32_t size : {1u,2u,8u}) {
        auto candidate = ready; value = 80;
        assert(candidate.access(eim,0x200ac300,0x16010220,size,true,value) == A::width);
    }
    for (uint32_t producer : {64u,16384u,0xffffffffu}) {
        auto candidate = ready; value = producer;
        assert(candidate.access(eim,0x200ac300,0x16010220,4,true,value) == A::sideEffect);
        assert(candidate.engineState().emptyRing() && !candidate.engineState().hostDataSubmission());
    }
    auto cancelled = ready; auto cancelledWork = readyCommands;
    assert(prepare(small,cancelled,cancelledWork) == A::ok);
    cancelled.startEngine(cancelledWork.duration()); cancelled.reset(); cancelledWork.reset();
    assert(cancelledWork.complete(cancelled,eim,memory) == A::sequence && read(0xf51000) == 0);
    assert(!cancelled.engineState().hostDataSubmission());
    Imx1Cs5Configuration strict;
    value = 80;
    assert(strict.access(eim,0x200ac300,0x16010220,4,true,value) != A::ok);
    std::puts("PASS: bounded native HOST_DATA_BLT RGB565, owned row pixels, clipping, timing and atomic rejection");
}

// Packed 1bpp host data. Payload shape follows the original producer and writer;
// drawing is the named virtual policy and is only checked as that policy.
inline void checkPackedMono(const Imx1Cs5Configuration& boot, const Imx1GraphicsCommands& bootCommands,
                            const Imx1Eim& eim) {
    using A = Imx1BootstrapAccess;
    auto state = boot;
    auto commands = bootCommands;
    Imx1GraphicsMemory memory;
    auto put = [&](uint32_t index, uint32_t word) {
        assert(memory.accessGpu(state,eim,0x150000+(index & 16383)*4,4,true,word) == A::ok);
    };
    std::vector<uint32_t> bridge;
    for (int i = 0; i < 5; ++i)
        for (uint32_t w : {0x4023841fu,0xffffu,0x014001e0u}) bridge.push_back(w);
    bridge.push_back(0x4a0); bridge.push_back(0);
    for (unsigned i = 0; i < bridge.size(); ++i) put(47+i,bridge[i]);
    uint32_t value = 64;
    assert(state.access(eim,0x200b0200,0x16010220,4,true,value) == A::ok);
    assert(commands.prepare(state,eim,memory) == A::ok);
    state.startEngine(commands.duration());
    assert(state.advanceCpuCycles(uint32_t(commands.duration())));
    assert(commands.complete(state,eim,memory) == A::ok && state.engineState().consumer() == 64);
    const auto ready = state;
    const auto readyCommands = commands;
    const uint32_t fg = 0x001f, bg = 0xffe0, base = 0xf51000;
    // RGB565 host words first, then the GMC-only change the producer emits for format 0.
    auto header = [&](uint32_t topLeft, uint32_t bottomRight, uint32_t gmc = 0x579804df) {
        return std::vector<uint32_t>{0x4025941b,0x5798b4df,0x00cc0b00,0x41b,gmc,
            0x40200c70,bottomRight,base,0x402,0x140,0x46f,topLeft,0x490,0xabcd0000u|fg,0x491,0x12340000u|bg};
    };
    // Writer 200b7984: two little-endian source halfwords per word, low then high.
    auto words = [](const std::vector<uint8_t>& bytes) {
        std::vector<uint32_t> out;
        for (size_t at = 0; at < bytes.size(); at += 4) {
            const uint32_t low = bytes[at] | uint32_t(bytes[at+1]) << 8;
            const uint32_t high = bytes[at+2] | uint32_t(bytes[at+3]) << 8;
            out.push_back(low | high << 16);
        }
        return out;
    };
    auto packet = [&](std::vector<uint32_t> batch, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      const std::vector<uint32_t>& payload) {
        batch.push_back(0xc0001900u | ((width/32*height+1) << 16));
        batch.push_back(y << 16 | x); batch.push_back(height << 16 | width);
        batch.insert(batch.end(),payload.begin(),payload.end());
        return batch;
    };
    auto submit = [&](const std::vector<uint32_t>& batch, Imx1Cs5Configuration& candidate, Imx1GraphicsCommands& work,
                      uint32_t pc = 0x200ac300) {
        for (unsigned i = 0; i < batch.size(); ++i) put(64+i,batch[i]);
        uint32_t producer = (64+uint32_t(batch.size())) & 16383;
        const auto status = candidate.access(eim,pc,0x16010220,4,true,producer);
        return status == A::ok ? work.prepare(candidate,eim,memory) : status;
    };
    auto read = [&](uint32_t x, uint32_t y) {
        uint32_t v = 0;
        assert(memory.accessGpu(ready,eim,base+y*640+x*2,2,false,v) == A::ok);
        return v;
    };
    auto paint = [&](uint32_t colour) {
        for (uint32_t i = 0; i < 110*640; i += 4) {
            uint32_t v = colour | colour << 16;
            assert(memory.accessGpu(ready,eim,base+i,4,true,v) == A::ok);
        }
    };
    auto reject = [&](const std::vector<uint32_t>& batch) {
        auto candidate = ready; auto work = readyCommands;
        for (unsigned i = 0; i < batch.size(); ++i) put(64+i,batch[i]);
        const auto internal = memory.dirty(0), external = memory.dirty(1);
        uint32_t producer = (64+uint32_t(batch.size())) & 16383;
        assert(candidate.access(eim,0x200ac300,0x16010220,4,true,producer) == A::ok);
        assert(work.prepare(candidate,eim,memory) != A::ok);
        assert(work.duration() == readyCommands.duration());
        assert(memory.dirty(0).generation == internal.generation && memory.dirty(1).generation == external.generation);
    };
    auto run = [&](const std::vector<uint32_t>& batch, uint64_t pixels) {
        auto candidate = ready; auto work = readyCommands;
        assert(submit(batch,candidate,work) == A::ok);
        assert(work.duration() == batch.size()+1+pixels);
        assert(candidate.engineState().hostDataSubmission() && candidate.engineState().consumer() == 64);
        // Pending pixels and colours are owned by the command, with the ring overwritten.
        for (unsigned i = 0; i < batch.size(); ++i) put(64+i,0);
        candidate.startEngine(work.duration());
        assert(candidate.advanceCpuCycles(uint32_t(work.duration()-1)));
        assert(work.complete(candidate,eim,memory) == A::sequence);
        assert(candidate.advanceCpuCycles(1) && work.complete(candidate,eim,memory) == A::ok);
        assert(candidate.engineState().emptyRing());
    };
    // 84x84 at 1bpp: 12 source bytes a row, with the 12 padding bits set to show they stay hidden.
    std::vector<uint8_t> source(84*12);
    uint32_t seed = 0x1100;
    for (auto& byte : source) { seed = seed*1103515245u+12345u; byte = uint8_t(seed >> 16); }
    for (unsigned y = 0; y < 84; ++y) { source[y*12+10] |= 0x0f; source[y*12+11] = 0xff; }
    const auto payload = words(source);
    const auto whole = packet(header(0x00140064,0x006800b8),100,20,96,84,payload);
    assert(payload.size() == 252 && whole[16] == 0xc0fd1900 && whole[18] == 0x00540060);
    assert(!Imx1Cs5Configuration(Imx1GraphicsProfile::strict,false,true).packedMonoEnabled());
    {
        // The mix shadow follows GMC 579804df only under the policy. A solid fill after it
        // still sees the boot PATCOPY mix when the policy is off.
        auto candidate = ready; auto work = readyCommands;
        const auto status = submit({0x41b,0x579804df,0x41b,0x55e0a4df,0xc0011a00,0,0x00020001},
                                   candidate,work,0x200b0200);
        assert((status == A::ok) == !boot.packedMonoEnabled());
        // A packet cut off before its coordinates is a sequence stop either way.
        auto cut = header(0x00140064,0x006800b8);
        cut.push_back(0xc0fd1900);
        candidate = ready; work = readyCommands;
        assert(submit(cut,candidate,work) == A::sequence);
        cut.push_back(0x00140064);
        candidate = ready; work = readyCommands;
        assert(submit(cut,candidate,work) == A::sequence);
    }
    if (!boot.packedMonoEnabled()) {
        // Without the declared policy the producer's packed rows stay guarded.
        reject(whole);
        std::puts("PASS: packed 1bpp host data is guarded without the virtual policy");
        return;
    }
    auto bit = [&](unsigned x, unsigned y) { return (source[y*12+x/8] >> (7-x%8)) & 1; };
    auto checkImage = [&]() {
        for (unsigned y = 0; y < 84; ++y) {
            for (unsigned x = 0; x < 84; ++x) assert(read(100+x,20+y) == (bit(x,y) ? fg : bg));
            // Padding columns 84..95 and both neighbours keep the destination.
            for (unsigned x : {99u,184u,185u,195u,196u}) assert(read(x,20+y) == 0x1234);
        }
        for (unsigned x = 99; x < 197; ++x) assert(read(x,19) == 0x1234 && read(x,104) == 0x1234);
    };
    paint(0x1234);
    run(whole,84*84);
    checkImage();
    // Synthetic 40/44 split. The 200abde4 row loop (native-submit-pseudo.c, zodiac-after-splash-budget-20261108)
    // sets the scissor once, then commits whole-row parts at 200ac300 with Y advanced.
    // Rows per part come from 200c78b4 and the ring space, which this test does not model.
    paint(0x1234);
    const std::vector<uint32_t> upper(payload.begin(),payload.begin()+40*3), lower(payload.begin()+40*3,payload.end());
    run(packet(header(0x00140064,0x006800b8),100,20,96,40,upper),84*40);
    for (unsigned x = 100; x < 184; ++x) assert(read(x,60) == 0x1234);
    run(packet(header(0x00140064,0x006800b8),100,60,96,44,lower),84*44);
    checkImage();
    // virtualPackedMonoMsbFirst and virtualPackedMonoSetBitForeground, one bit at a time.
    for (unsigned sample = 0; sample < 32; ++sample) {
        std::vector<uint8_t> row(4);
        row[sample/8] = uint8_t(0x80 >> (sample%8));
        paint(0x1234);
        run(packet(header(0x00050020,0x00060040),32,5,32,1,words(row)),32);
        // virtualPackedMonoOpaqueCopy: clear bits replace the destination with the background.
        for (unsigned x = 0; x < 32; ++x) assert(read(32+x,5) == (x == sample ? fg : bg));
        assert(read(31,5) == 0x1234 && read(64,5) == 0x1234 && read(32,4) == 0x1234 && read(32,6) == 0x1234);
    }
    // virtualPackedMonoExclusiveScissor: right 37 and bottom 6 are the first column and row not drawn.
    paint(0x1234);
    run(packet(header(0x00050020,0x00060025),32,5,32,2,{0xffffffff,0xffffffff}),5);
    for (unsigned x = 32; x < 37; ++x) assert(read(x,5) == fg && read(x,6) == 0x1234);
    assert(read(37,5) == 0x1234);
    // virtualPackedMonoZeroPhase: a row aligned to source x 0 at 96 with the left scissor at 100
    // puts sample 4 on column 100.
    paint(0x1234);
    run(packet(header(0x00050064,0x00060078),96,5,32,1,words({0xf5,0xa0,0xff,0x00})),20);
    for (unsigned x = 96; x < 100; ++x) assert(read(x,5) == 0x1234);
    for (unsigned x = 100; x < 112; ++x) {
        const unsigned sample = x-96;
        assert(read(x,5) == ((sample < 8 ? 0xf5 >> (7-sample) : 0xa0 >> (15-sample)) & 1 ? fg : bg));
    }
    for (unsigned x = 112; x < 120; ++x) assert(read(x,5) == fg);
    assert(read(120,5) == 0x1234);
    // An explicit datatype write with source field 0 is also admitted.
    auto typed = header(0x00050020,0x00060040);
    typed.insert(typed.end(),{0x4b1,0x40000d04});
    paint(0x1234);
    run(packet(typed,32,5,32,1,{0}),32);
    assert(read(32,5) == bg);
    paint(0x1234);
    const std::vector<uint32_t> two{0x000000ff,0xff000000};
    // Row shape: widths that are not whole words, wrong counts and short or long payloads.
    for (uint32_t width : {84u,31u,33u,16u,0u}) {
        auto bad = packet(header(0x00050020,0x00060040),32,5,32,2,two); bad[18] = 2u << 16 | width; reject(bad);
    }
    for (uint32_t count : {2u,4u,5u}) {
        auto bad = packet(header(0x00050020,0x00060040),32,5,32,2,two); bad[16] = 0xc0001900u | count << 16; reject(bad);
    }
    { auto bad = packet(header(0x00050020,0x00060040),32,5,32,2,two); bad.pop_back(); reject(bad); }
    { auto bad = packet(header(0x00050020,0x00060040),32,5,32,2,two); bad.push_back(0); reject(bad); }
    // Source field 1, other ROPs and other datatype words are outside the declared policy.
    reject(packet(header(0x00050020,0x00060040,0x579814df),32,5,32,2,two));
    { auto bad = packet(header(0x00050020,0x00060040,0x579814df),32,5,32,2,std::vector<uint32_t>(32,0));
      bad[16] = 0xc0211900; reject(bad); }
    reject(packet(header(0x00050020,0x00060040,0x57e004df),32,5,32,2,two));
    // 579884df was a static guess that the run did not show.
    reject(packet(header(0x00050020,0x00060040,0x579884df),32,5,32,2,two));
    for (uint32_t datatype : {0x40000c04u,0x00000d04u,0x40000d06u}) {
        auto bad = header(0x00050020,0x00060040);
        bad.insert(bad.end(),{0x4b1,datatype});
        reject(packet(bad,32,5,32,2,two));
    }
    // RGB565 rows keep their own guard on an interior upper scissor edge.
    { auto bad = packet(header(0x00050020,0x00060025,0x5798b4df),32,5,32,2,std::vector<uint32_t>(32,0));
      bad[16] = 0xc0211900; reject(bad);
      bad[6] = 0x00070040; auto candidate = ready; auto work = readyCommands;
      assert(submit(bad,candidate,work) == A::ok); }
    for (unsigned x = 0; x < 320; ++x) for (unsigned y = 0; y < 110; ++y) assert(read(x,y) == 0x1234);
    {
        // Replay of the 9 November 2026 empty-card run's terminal submission (steps 411540176 on):
        // its eleven ring words after the run's shadow state, with a generated payload.
        auto candidate = ready; auto work = readyCommands;
        const std::vector<uint32_t> shadow{0x401,0x100000,0x402,0x1e0,0x4025941b,0x5798b4df,0x00cc0b00,
            0x4b1,0x40030d04,0x472,0x3f,0x488,4,0x4a0,0,0x4a1,0};
        assert(submit(shadow,candidate,work,0x200b0200) == A::ok);
        candidate.startEngine(work.duration());
        assert(candidate.advanceCpuCycles(uint32_t(work.duration())) && work.complete(candidate,eim,memory) == A::ok);
        std::vector<uint32_t> witness{0x4024841b,0x579804df,0x00000000,0x40237c91,0x0000ffff,0x00de014e,
            0x00000470,0x013201a2,0xc0fd1900,0x00de014e,0x00540060};
        witness.insert(witness.end(),payload.begin(),payload.end());
        const uint32_t begin = 64+uint32_t(shadow.size());
        for (unsigned i = 0; i < witness.size(); ++i) put(begin+i,witness[i]);
        auto pixel = [&](uint32_t x, uint32_t y, bool write = false, uint32_t v = 0) {
            assert(memory.accessGpu(ready,eim,0x100000+y*960+x*2,2,write,v) == A::ok);
            return v;
        };
        for (unsigned y = 221; y < 307; ++y) for (unsigned x = 333; x < 431; ++x) pixel(x,y,true,0x1234);
        uint32_t producer = begin+uint32_t(witness.size());
        assert(candidate.access(eim,0x200ac300,0x16010220,4,true,producer) == A::ok);
        assert(work.prepare(candidate,eim,memory) == A::ok);
        assert(work.duration() == witness.size()+1+84*84);
        candidate.startEngine(work.duration());
        assert(candidate.advanceCpuCycles(uint32_t(work.duration())) && work.complete(candidate,eim,memory) == A::ok);
        assert(candidate.engineState().emptyRing());
        // 84x84 at 334,222 on the 480 pitch: set bits in register 490 (0), clear bits in 491 (ffff).
        for (unsigned y = 0; y < 84; ++y) {
            for (unsigned x = 0; x < 84; ++x) assert(pixel(334+x,222+y) == (bit(x,y) ? 0u : 0xffffu));
            for (unsigned x : {333u,418u,429u,430u}) assert(pixel(x,222+y) == 0x1234);
        }
        for (unsigned x = 333; x < 431; ++x) assert(pixel(x,221) == 0x1234 && pixel(x,306) == 0x1234);
    }
    std::puts("PASS: packed 1bpp host rows, 84 in 96 with split parts, under the declared virtual policy "
              "(MsbFirst, SetBitForeground, OpaqueCopy, ExclusiveScissor, ZeroPhase)");
}
#endif
