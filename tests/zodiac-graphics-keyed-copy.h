#ifndef FROND_TEST_ZODIAC_GRAPHICS_KEYED_COPY_H
#define FROND_TEST_ZODIAC_GRAPHICS_KEYED_COPY_H

inline void checkKeyedCopy(const Imx1Cs5Configuration& ready, const Imx1GraphicsCommands& readyCommands,
                           const Imx1Eim& eim) {
    using A = Imx1BootstrapAccess;
    Imx1GraphicsMemory memory;
    auto read = [&](uint32_t address, uint32_t size = 2) {
        uint32_t value = 0;
        assert(memory.accessGpu(ready,eim,address,size,false,value) == A::ok);
        return value;
    };
    auto write = [&](uint32_t address, uint32_t value, uint32_t size = 2) {
        assert(memory.accessGpu(ready,eim,address,size,true,value) == A::ok);
    };
    auto load = [&](uint32_t first, const std::vector<uint32_t>& words) {
        for (unsigned i = 0; i < words.size(); ++i) write(0x150000+((first+i)&16383)*4,words[i],4);
    };
    const std::vector<uint16_t> pixels{0xf800,0x1234,0xabcd,0xf800,0x07e0,0xf800,0x001f,0x4567,
        0x89ab,0xcdef,0xf800,0x1357};
    std::vector<uint32_t> host{0x4025941b,0x5798b4df,0x00cc0b00,0x40200c70,0x00030004,0x00f51000,
        0x402,0x140,0xc0071900,0,0x00030004};
    for (unsigned i = 0; i < pixels.size(); i += 2) host.push_back(pixels[i] | uint32_t(pixels[i+1]) << 16);
    const std::vector<uint32_t> copy{0x4023841b,0x4598b4df,0x014001e0,
        0x40201401,0x00100000,0x000001e0,0x4023646b,0x00f51000,0x00000140,
        0x48f,0xffffffff,0xc0051c00,0x01000005,0xf800,0,0,0x00c400b4,0x00040003};
    constexpr uint32_t destination = 0x100000+(180*480+196)*2;
    auto clear = [&]() {
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                write(0xf51000+y*640+x*2,0);
                write(destination+y*960+x*2,0xa55a);
            }
    };
    auto append = [&](Imx1Cs5Configuration& state, Imx1GraphicsCommands& commands,
                      const std::vector<uint32_t>& batch, uint32_t pc) {
        const auto first = state.engineState().producer();
        load(first,batch);
        auto candidate = state;
        uint32_t value = (first+uint32_t(batch.size()))&16383;
        auto status = candidate.access(eim,pc,0x16010220,4,true,value);
        if (status != A::ok) return status;
        status = commands.prepare(candidate,eim,memory);
        if (status != A::ok) return status;
        candidate.startEngine(commands.duration());
        state = std::move(candidate);
        return A::ok;
    };
    auto check = [&](bool clipped = false) {
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                const uint32_t colour = x < 4 && y < 3 ? pixels[y*4+x] : 0xf800;
                const uint32_t expected = colour == 0xf800 || (clipped && (!x || !y)) ? 0xa55a : colour;
                assert(read(destination+y*960+x*2) == expected);
                if (x < 4 && y < 3) assert(read(0xf51000+y*640+x*2) == pixels[y*4+x]);
            }
        assert(read(destination+2,1) == 0x34 && read(destination+3,1) == 0x12);
    };
    for (bool singleAdvance : {false,true}) {
        clear();
        auto state = ready;
        auto commands = readyCommands;
        const auto initialConsumer = state.engineState().consumer();
        assert(append(state,commands,host,0x200ac300) == A::ok);
        const auto uploadEnd = state.engineState().producer();
        const auto uploadTicks = commands.duration();
        assert(uploadTicks == host.size()+1+pixels.size());
        assert(state.advanceCpuCycles(7));
        assert(append(state,commands,copy,0x200aa504) == A::ok);
        const auto copyEnd = state.engineState().producer();
        const auto copyTicks = commands.duration();
        assert(copyTicks == copy.size()+1+pixels.size());
        assert(state.engineState().consumer() == initialConsumer);
        assert(read(0xf51002) == 0 && read(destination+2) == 0xa55a);
        const auto pending = state.interruptState().pendingValue();
        uint32_t value = 0;
        assert(state.access(eim,0x200b8cb0,0x16010140,4,false,value) == A::ok && value == 0x80000000);
        value = 0;
        assert(state.access(eim,0x200b02ac,0x16011280,4,false,value) == A::sequence);
        assert(state.access(eim,0x200b32b0,0x16010080,4,true,value) == A::sequence);
        load(initialConsumer,std::vector<uint32_t>(host.size()+copy.size(),0xdeadbeef));
        auto owned = commands;
        commands.reset();
        if (singleAdvance) {
            assert(state.advanceCpuCycles(uint32_t(uploadTicks-7+copyTicks+100)));
            assert(owned.complete(state,eim,memory) == A::ok);
        } else {
            assert(state.advanceCpuCycles(uint32_t(uploadTicks-8)) && !state.engineState().completed());
            assert(owned.complete(state,eim,memory) == A::sequence);
            assert(read(0xf51002) == 0);
            assert(state.advanceCpuCycles(1) && state.engineState().completed());
            assert(owned.complete(state,eim,memory) == A::ok);
            assert(state.engineState().consumer() == uploadEnd && !state.engineState().emptyRing());
            assert(read(0xf51002) == 0x1234 && read(destination+2) == 0xa55a);
            assert(state.advanceCpuCycles(uint32_t(copyTicks-1)) && !state.engineState().completed());
            assert(state.advanceCpuCycles(1) && owned.complete(state,eim,memory) == A::ok);
        }
        assert(state.engineState().consumer() == copyEnd && state.engineState().emptyRing());
        assert(state.interruptState().pendingValue() == pending);
        check();
        // Switching back to host data retains ownership of the earlier queued memory copy.
        assert(append(state,owned,host,0x200ac300) == A::ok);
        assert(state.advanceCpuCycles(uint32_t(owned.duration())) && owned.complete(state,eim,memory) == A::ok);
    }
    // The bus discards preflight candidates and rebuilds them after charging wait clocks.
    clear();
    auto waited = ready;
    auto waitedCommands = readyCommands;
    assert(append(waited,waitedCommands,host,0x200ac300) == A::ok);
    auto preview = waited;
    auto previewCommands = waitedCommands;
    assert(append(preview,previewCommands,copy,0x200aa504) == A::ok);
    const auto hostEnd = waited.engineState().producer();
    assert(waited.advanceCpuCycles(uint32_t(waitedCommands.duration())));
    assert(waitedCommands.complete(waited,eim,memory) == A::ok);
    assert(waited.engineState().consumer() == hostEnd && read(0xf51002) == 0x1234);
    assert(append(waited,waitedCommands,copy,0x200aa504) == A::ok);
    assert(waited.advanceCpuCycles(uint32_t(waitedCommands.duration())));
    assert(waitedCommands.complete(waited,eim,memory) == A::ok && waited.engineState().emptyRing());
    check();
    // Rejected appends preserve all previously accepted command/register/timing state.
    clear();
    auto state = ready;
    auto commands = readyCommands;
    assert(append(state,commands,host,0x200ac300) == A::ok);
    const auto uploadTicks = commands.duration();
    const auto producer = state.engineState().producer();
    const auto dirty = memory.dirty(1).generation;
    const auto clock = state.clockState().count();
    const auto pending = state.interruptState().pendingValue();
    auto reject = [&](std::vector<uint32_t> bad, uint32_t pc = 0x200aa504) {
        const auto status = append(state,commands,bad,pc);
        if (status == A::ok) {
            std::fprintf(stderr,"Unexpected accepted keyed batch (%zu words):",bad.size());
            for (auto word : bad) std::fprintf(stderr," %08x",word);
            std::fprintf(stderr,"\n");
        }
        assert(status != A::ok);
        assert(state.engineState().producer() == producer && state.engineState().consumer() == ready.engineState().consumer());
        assert(commands.duration() == uploadTicks && memory.dirty(1).generation == dirty);
        assert(state.clockState().count() == clock && state.interruptState().pendingValue() == pending);
        assert(read(0xf51002) == 0 && read(destination+2) == 0xa55a);
    };
    for (auto change : {std::pair<unsigned,uint32_t>{1,0x4598b4de},{4,0xfffffffe},
         {5,0},{5,8193},{7,0xf50fff},{8,0},{8,3},{8,8193},{10,0xffff},{11,0xc0041c00},
         {12,0x01000004},{12,0x02000005},{13,0x10000},{14,1},{15,0xffff0000},{15,0x013f0000},
         {16,0xffffffff},{17,0},{17,0xffff0001}}) {
        auto bad = copy; bad[change.first] = change.second; reject(bad);
    }
    for (unsigned length = 0; length < copy.size(); ++length) reject({copy.begin(),copy.begin()+length});
    auto bad = copy; bad.push_back(0); reject(bad);
    bad = copy; bad.insert(bad.begin()+11,{0x4b2,0x00cc0b00}); reject(bad); // explicit DP_MIX after GMC wins
    bad = copy; bad[7] = destination; reject(bad); // overlapping memory
    for (uint32_t reg : {0x4a1u,0x42eu,0x488u,0x4b1u,0x472u,0x471u}) {
        bad = copy; bad.insert(bad.begin()+11,{reg,1}); reject(bad);
    }
    reject(copy,0x200aa508);
    for (uint32_t size : {1u,2u,8u}) {
        uint32_t end = producer+uint32_t(copy.size());
        assert(state.access(eim,0x200aa504,0x16010220,size,true,end) == A::width);
        assert(state.engineState().producer() == producer);
    }
    Imx1Cs5Configuration strict;
    uint32_t strictEnd = 18;
    assert(strict.access(eim,0x200aa504,0x16010220,4,true,strictEnd) != A::ok);
    assert(append(state,commands,copy,0x200aa504) == A::ok);
    assert(state.advanceCpuCycles(uint32_t(uploadTicks+commands.duration())));
    assert(commands.complete(state,eim,memory) == A::ok);
    check();
    // The text producer's colours in 490 and 491 don't change a keyed copy.
    clear(); state = ready; commands = readyCommands;
    assert(append(state,commands,host,0x200ac300) == A::ok);
    auto afterText = copy;
    afterText.insert(afterText.begin()+11,{0x490,0,0x491,0xffff});
    assert(append(state,commands,afterText,0x200aa504) == A::ok);
    assert(state.advanceCpuCycles(1000) && commands.complete(state,eim,memory) == A::ok);
    check();
    // Any other pair in 490 and 491 is refused.
    for (const std::vector<uint32_t>& colours : {std::vector<uint32_t>{0x490,0}, {0x491,0xffff}, {0x490,0x1234,0x491,0xffff}}) {
        clear(); state = ready; commands = readyCommands;
        assert(append(state,commands,host,0x200ac300) == A::ok);
        auto other = copy;
        other.insert(other.begin()+11,colours.begin(),colours.end());
        assert(append(state,commands,other,0x200aa504) == A::sideEffect);
    }
    // Left/top clipping advances both the source and destination with separate row strides.
    clear(); state = ready; commands = readyCommands;
    assert(append(state,commands,host,0x200ac300) == A::ok);
    auto clipped = copy;
    clipped.insert(clipped.begin()+11,{0x46f,0x00b500c5});
    // An earlier explicit mix write is replaced by the later qualified GMC source selection.
    clipped.insert(clipped.begin(),{0x4b2,0x00cc0b00});
    assert(append(state,commands,clipped,0x200aa504) == A::ok);
    assert(commands.duration() == clipped.size()+1+6);
    assert(state.advanceCpuCycles(1000) && commands.complete(state,eim,memory) == A::ok);
    for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            assert(read(destination+y*960+x*2) == (!x || !y || pixels[y*4+x] == 0xf800 ? 0xa55a : pixels[y*4+x]));
    clear(); state = ready; commands = readyCommands;
    assert(append(state,commands,host,0x200ac300) == A::ok);
    auto offsetCopy = copy;
    offsetCopy[15] = 0x00020001; offsetCopy[17] = 0x00020002;
    assert(append(state,commands,offsetCopy,0x200aa504) == A::ok);
    assert(state.advanceCpuCycles(1000) && commands.complete(state,eim,memory) == A::ok);
    for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t pixel = x < 2 && y < 2 ? pixels[(y+1)*4+x+2] : 0xf800;
            assert(read(destination+y*960+x*2) == (pixel == 0xf800 ? 0xa55a : pixel));
        }
    // The optional private packet contains no fixture bytes in the repository.
    if (const auto path = std::getenv("FROND_ZODIAC_HOSTDATA_PACKET")) {
        auto bytes = [](const char* name) {
            std::ifstream file(name,std::ios::binary);
            assert(file);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),{});
        };
        const auto packet = bytes(path), source = bytes(std::getenv("FROND_ZODIAC_HOSTDATA_SOURCE"));
        assert(packet.size() == 759*4 && source.size() == 44*34*2);
        std::vector<uint32_t> captured;
        for (size_t at = 0; at < packet.size(); at += 4)
            captured.push_back(uint32_t(packet[at]) | uint32_t(packet[at+1]) << 8 |
                uint32_t(packet[at+2]) << 16 | uint32_t(packet[at+3]) << 24);
        state = ready; commands = readyCommands;
        std::vector<uint32_t> bridge;
        for (unsigned i = 0; i < 5; ++i)
            for (uint32_t word : {0x4023841fu,0xffffu,0x014001e0u}) bridge.push_back(word);
        bridge.insert(bridge.end(),{0x4a0,0});
        assert(append(state,commands,bridge,0x200b0200) == A::ok);
        assert(state.advanceCpuCycles(uint32_t(commands.duration())) && commands.complete(state,eim,memory) == A::ok);
        assert(state.engineState().consumer() == 64);
        for (uint32_t y = 0; y < 34; ++y)
            for (uint32_t x = 0; x < 44; ++x) {
                write(0xf51000+y*640+x*2,0);
                write(destination+y*960+x*2,0xa55a);
            }
        assert(append(state,commands,captured,0x200ac300) == A::ok && commands.duration() == 2256);
        assert(state.engineState().producer() == 0x337);
        assert(state.advanceCpuCycles(7));
        auto nativeCopy = copy; nativeCopy[17] = 0x002c0022;
        assert(append(state,commands,nativeCopy,0x200aa504) == A::ok && commands.duration() == 1515);
        assert(state.engineState().producer() == 0x349 && read(0xf51002) == 0);
        load(64,std::vector<uint32_t>(759+18,0));
        assert(state.advanceCpuCycles(2249) && commands.complete(state,eim,memory) == A::ok);
        assert(state.engineState().consumer() == 0x337 && !state.engineState().emptyRing());
        for (uint32_t y = 0; y < 34; ++y)
            for (uint32_t x = 0; x < 44; ++x) {
                const auto at = (y*44+x)*2;
                assert(read(0xf51000+y*640+x*2) == (uint32_t(source[at]) << 8 | source[at+1]));
                assert(read(destination+y*960+x*2) == 0xa55a);
            }
        assert(state.advanceCpuCycles(1515) && commands.complete(state,eim,memory) == A::ok);
        assert(state.engineState().consumer() == 0x349 && state.engineState().emptyRing());
        for (uint32_t y = 0; y < 34; ++y)
            for (uint32_t x = 0; x < 44; ++x) {
                const auto at = (y*44+x)*2;
                const uint32_t pixel = uint32_t(source[at]) << 8 | source[at+1];
                assert(read(destination+y*960+x*2) == (pixel == 0xf800 ? 0xa55a : pixel));
            }
        std::puts("PASS: private captured upload337 then keyed349, every1496 pixels against independent BE source");
    }
    // Reset cancels both pending batches and retains writes completed before reset.
    for (bool finishUpload : {false,true}) {
        clear(); state = ready; commands = readyCommands;
        assert(append(state,commands,host,0x200ac300) == A::ok);
        const auto ticks = commands.duration();
        assert(append(state,commands,copy,0x200aa504) == A::ok);
        if (finishUpload) assert(state.advanceCpuCycles(uint32_t(ticks)) && commands.complete(state,eim,memory) == A::ok);
        state.reset(); commands.reset();
        assert(state.advanceCpuCycles(1000) && !state.engineState().completed());
        assert(commands.complete(state,eim,memory) == A::sequence);
        assert(read(0xf51002) == (finishUpload ? 0x1234 : 0) && read(destination+2) == 0xa55a);
    }
    clear(); state = ready; commands = readyCommands;
    assert(append(state,commands,host,0x200ac300) == A::ok);
    state.reset(); commands.reset();
    assert(append(state,commands,copy,0x200aa504) == A::sequence);
    assert(read(0xf51002) == 0 && read(destination+2) == 0xa55a);
    // A parsed batch can wrap; a following append cannot reparse its owned words.
    state = ready; commands = readyCommands;
    std::vector<uint32_t> padding;
    const uint32_t paddingWords = 16382-ready.engineState().producer();
    assert(paddingWords % 2 == 1);
    padding.insert(padding.end(),{0x4023841fu,0xffff,0x014001e0});
    for (uint32_t i = 3; i < paddingWords; i += 2) padding.insert(padding.end(),{0x41f,0xffff});
    assert(append(state,commands,padding,0x200b0200) == A::ok);
    assert(state.advanceCpuCycles(uint32_t(commands.duration())) && commands.complete(state,eim,memory) == A::ok);
    assert(state.engineState().consumer() == 16382);
    assert(append(state,commands,host,0x200ac300) == A::ok);
    assert(append(state,commands,copy,0x200aa504) == A::ok);
    assert(state.advanceCpuCycles(1000) && commands.complete(state,eim,memory) == A::ok);
    check();
    clear(); state = ready; commands = readyCommands;
    assert(append(state,commands,padding,0x200b0200) == A::ok);
    const auto paddingTicks = commands.duration();
    assert(append(state,commands,host,0x200ac300) == A::ok);
    const auto hostTicks = commands.duration();
    assert(append(state,commands,copy,0x200aa504) == A::ok);
    const auto copyTicks = commands.duration();
    const auto queuedEnd = state.engineState().producer();
    assert(append(state,commands,host,0x200ac300) == A::sideEffect);
    assert(commands.duration() == copyTicks && state.engineState().producer() == queuedEnd);
    assert(state.advanceCpuCycles(uint32_t(paddingTicks+hostTicks+copyTicks)));
    assert(commands.complete(state,eim,memory) == A::ok && state.engineState().emptyRing());
    check();
    const auto end = state.engineState().consumer();
    uint32_t noOp = end;
    assert(state.access(eim,0x200b0200,0x16010220,4,true,noOp) == A::ok);
    assert(commands.prepare(state,eim,memory) == A::ok);
    state.startEngine(commands.duration());
    assert(state.advanceCpuCycles(1000) && state.engineState().emptyRing() && !state.engineState().completed());
    // One ring word stays free even when the producer wraps past the consumer.
    state = ready;
    uint32_t value = (state.engineState().producer()-1)&16383;
    assert(state.access(eim,0x200aa504,0x16010220,4,true,value) == A::ok);
    state.startEngine(100);
    const auto fullProducer = state.engineState().producer();
    for (uint32_t end : {ready.engineState().consumer(),(fullProducer+2)&16383,16384u,0xffffffffu}) {
        value = end;
        assert(state.access(eim,0x200aa504,0x16010220,4,true,value) == A::sideEffect);
        assert(state.engineState().producer() == fullProducer);
    }
    std::puts("PASS: queued host upload/keyed copy ordering, compare, strides, deadlines, ownership, bounds, atomic rejection and reset");
}
#endif
