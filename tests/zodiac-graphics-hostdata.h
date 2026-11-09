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
#endif
