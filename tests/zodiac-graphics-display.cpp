#include <cassert>
#include <cstdio>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_frame.h"

using A = Imx1BootstrapAccess;

static void store(Imx1Cs5Configuration& state, const Imx1Eim& eim, uint32_t pc, uint32_t offset, uint32_t value) {
    assert(state.access(eim, pc, 0x16010000 + offset, 4, true, value) == A::ok);
}
static void engine(Imx1Cs5Configuration& state, const Imx1Eim& eim) {
    uint32_t value = 0;
    assert(state.access(eim, 0x200acb98, 0x160113e8, 4, false, value) == A::ok);
    for (auto op : {std::array<uint32_t,3>{0x200acba8,0x13e8,0x400},
            {0x200acbb4,0x144,0x2440},{0x200acbc4,0x10a8,0x1fff1fff},{0x200acbd0,0x4dc,0xc0000},
            {0x200b8e80,0x214,0x10150000},{0x200b8e88,0x220,0},{0x200b8f04,8,0x04150000},
            {0x200b8f10,0x230,0x80000000},{0x200b8f20,0x210,0x0800000d}})
        store(state,eim,op[0],op[1],op[2]);
}
static void transaction(Imx1Cs5Configuration& state, const Imx1Eim& eim, bool begin) {
    store(state,eim,0x200aeb68,0x4d8,begin ? 0x78 : 0x7b);
}
static void mode(Imx1Cs5Configuration& state, const Imx1Eim& eim, uint32_t base = 0x800000) {
    transaction(state,eim,true);
    store(state,eim,0x200ad720,0x418,base);
    store(state,eim,0x200ad72c,0x41c,8);
    store(state,eim,0x200ad738,0x414,6);
    transaction(state,eim,false);
    uint32_t value = 0;
    assert(state.access(eim,0x200ad74c,0x160104d8,4,false,value) == A::ok && (value & 2));
    assert(state.displayState().activeWord(0x418) == 0);
    state.advanceCpuCycles(31);
    assert(state.displayState().latchPending());
    state.advanceCpuCycles(1);
    assert(!state.displayState().latchPending() && state.displayState().activeWord(0x418) == base);
    transaction(state,eim,true);
    store(state,eim,0x200ad65c,0x42c,0x40001);
    store(state,eim,0x200ad668,0x430,0x30001);
    for (auto op : {std::array<uint32_t,3>{0x200ad2e0,0x410,3},
            {0x200ad404,0x540,0x80604020},{0x200ad410,0x544,0xffdfbf9f},{0x200ad418,0x548,0},
            {0x200ad420,0x578,0x80604020},{0x200ad428,0x58c,0xffdfbf9f},{0x200ad430,0x590,0},
            {0x200ad438,0x594,0x80604020},{0x200ad440,0x598,0xffdfbf9f},{0x200ad448,0x59c,0},
            {0x200ad528,0x538,0x800000},{0x200ad3b8,0x524,0x102aa}}) store(state,eim,op[0],op[1],op[2]);
    store(state,eim,0x200ad2ec,0x414,0x1800406);
    store(state,eim,0x200ad2f8,0x420,0x60008);
    store(state,eim,0x200ad304,0x424,0x40001);
    store(state,eim,0x200ad310,0x428,0x30001);
    transaction(state,eim,false);
    state.advanceCpuCycles(31);
    assert(state.displayState().clockAcknowledgements() == 0);
    state.advanceCpuCycles(1);
    assert(state.displayState().clockAcknowledgements() == 0x6000000);
    assert(!state.displayState().lcdEnabled());
    transaction(state,eim,true);
    store(state,eim,0x200ad8b4,0x414,0x1801c06);
    transaction(state,eim,false);
    state.advanceCpuCycles(32);
    assert(state.displayState().lcdEnabled());
}

int main() {
    Imx1Eim eim;
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028,4,true,value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c,4,true,value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration state(Imx1GraphicsProfile::virtual5744Compatible8MiB), strict;
    value = 0x78;
    assert(state.access(eim,0x200aeb68,0x160104d8,4,true,value) == A::sequence);
    assert(strict.access(eim,0x200aeb68,0x160104d8,4,true,value) == A::unmapped);
    configureGraphicsInterface(state,eim);
    configureGraphicsController(state,eim);
    engine(state,eim);
    assert(state.access(eim,0,0x160104d8,4,true,value) == A::externalInput);
    assert(state.access(eim,0x200aeb68,0x160104d8,2,true,value) == A::width);
    value = 0x7b;
    assert(state.access(eim,0x200aeb68,0x160104d8,4,true,value) == A::sequence);
    store(state,eim,0x200b99c4,0x4bc,0xffff1fff);
    store(state,eim,0x200b9a14,0x4b8,0x01ff0008);
    value = 0xdeadbeef;
    assert(state.access(eim,0x200b9cd4,0x160104c0,4,false,value,12) == A::externalInput && value == 0xdeadbeef);
    mode(state,eim);
    for (uint32_t pin = 9; pin <= 12; ++pin) {
        value = 0xdeadbeef;
        assert(state.access(eim,0x200b9cd4,0x160104c0,4,false,value,pin) == A::ok && value == 0);
    }
    value = 0xdeadbeef;
    assert(state.access(eim,0x200b9cd4,0x160104c0,4,false,value) == A::externalInput && value == 0xdeadbeef);
    assert(state.access(eim,0x200b9cd4,0x160104c0,4,false,value,13) == A::externalInput && value == 0xdeadbeef);
    assert(state.access(eim,0x200b9cd4,0x160104c0,4,false,value,3) == A::ok && value == 8);
    const uint64_t period = 8 * 6 * Imx1GraphicsDisplay::pixelCycles;
    assert(state.displayState().verticalBlankEvents() == 0);
    assert(state.interruptState().pendingValue() == 0);
    {
        auto rotated = state;
        Imx1GraphicsMemory bytes;
        Imx1GraphicsFrame output;
        unsigned n = 0;
        for (uint32_t colour : {0xf800u,0x07e0u,0x001fu,0xffffu,0u,0xffe0u}) {
            assert(bytes.accessGpu(rotated,eim,0x800000 + n++ * 2,2,true,colour) == A::ok);
        }
        transaction(rotated,eim,true);
        store(rotated,eim,0x200ae9a8,0x414,0x1801c16);
        store(rotated,eim,0x200ae9b8,0x418,0x800000);
        store(rotated,eim,0x200ae9c4,0x41c,4);
        transaction(rotated,eim,false);
        uint32_t pending = 0;
        assert(rotated.access(eim,0x200ae9d8,0x160104d8,4,false,pending) == A::ok && (pending & 2));
        rotated.advanceCpuCycles(period - 1);
        assert(rotated.displayState().latchPending() && rotated.displayState().activeWord(0x414) == 0x1801c06);
        rotated.advanceCpuCycles(1);
        assert(!rotated.displayState().latchPending());
        assert(output.advance(rotated,bytes,eim));
        uint32_t first = 99, last = 99;
        const auto pixels = output.getPendingFrame(first,last);
        assert(pixels && output.width() == 3 && output.height() == 2 && first == 0 && last == 1);
        const uint32_t expected[]{0xff00ff00,0xffffffff,0xff00ffff,0xff0000ff,0xffff0000,0xff000000};
        for (unsigned i = 0; i < 6; ++i) assert(pixels[i] == expected[i]);
        output.acknowledgeFrame();
        uint32_t blue = 0x001f;
        assert(bytes.accessGpu(rotated,eim,0x800008,2,true,blue) == A::ok);
        rotated.advanceCpuCycles(period);
        assert(output.advance(rotated,bytes,eim));
        assert(output.getPendingFrame(first,last)[5] == 0xffff0000);
        assert(first <= 1 && last >= 1);
        transaction(rotated,eim,true);
        uint32_t bad = 0x1801c0e;
        assert(rotated.access(eim,0x200ae9a8,0x16010414,4,true,bad) == A::sideEffect);
        store(rotated,eim,0x200ae9c4,0x41c,2);
        bad = 0x7b;
        assert(rotated.access(eim,0x200aeb68,0x160104d8,4,true,bad) == A::sideEffect);
        assert(!rotated.displayState().latchPending() && rotated.displayState().activeWord(0x41c) == 4);
        store(rotated,eim,0x200ae9c4,0x41c,4);
        store(rotated,eim,0x200ae9b8,0x418,0x800002);
        assert(rotated.access(eim,0x200aeb68,0x160104d8,4,true,bad) == A::sideEffect);
        bad = 0x12345678;
        assert(rotated.access(eim,0x200ae9dc,0x160104d8,4,false,bad) == A::externalInput && bad == 0x12345678);
    }
    {
        auto raster = state.displayState();
        const auto system = Imx1GraphicsClocks::Rate{200000000,1};
        raster.advanceCpuCycles(100);
        raster.setClockSources({0,1},system);
        raster.advanceCpuCycles(10000);
        assert(raster.scanPhase() == 100 && raster.verticalBlankEvents() == 0);
        raster.setClockSources({25000000,1},system); // Two phase units per 200 MHz tick.
        raster.advanceCpuCycles(141);
        assert(raster.scanPhase() == 382 && raster.verticalBlankEvents() == 0);
        raster.advanceCpuCycles(1);
        assert(raster.scanPhase() == 384 && raster.verticalBlankEvents() == 1);
        raster.setClockSources({12500000,1},system);
        raster.advanceCpuCycles(period);
        assert(raster.scanPhase() == 384 && raster.verticalBlankEvents() == 2);
    }
    {
        auto irq = state;
        irq.advanceCpuCycles(3 * 8 * Imx1GraphicsDisplay::pixelCycles - 1);
        assert(irq.interruptState().pendingValue() == 0);
        irq.advanceCpuCycles(1);
        assert(irq.displayState().verticalBlankEvents() == 1);
        assert(irq.interruptState().pendingValue() == 1);
        assert(irq.interruptState().interruptOutput() == Imx1GpioLine::clear);
        store(irq,eim,0x20086e08,0x200,1);
        assert(irq.interruptState().interruptOutput() == Imx1GpioLine::asserted);
        store(irq,eim,0x20087824,0x204,1);
        assert(irq.interruptState().pendingValue() == 0);
        irq.advanceCpuCycles(period - 1);
        assert(irq.interruptState().pendingValue() == 0);
        irq.advanceCpuCycles(1);
        assert(irq.displayState().verticalBlankEvents() == 2);
        assert(irq.interruptState().pendingValue() == 1);
        store(irq,eim,0x20087702,0x200,0);
        assert(irq.interruptState().pendingValue() == 1);
        assert(irq.interruptState().interruptOutput() == Imx1GpioLine::clear);
    }

    {
        auto waited=state;
        store(waited,eim,0x200c45e4,0x80,0x3f);
        auto partition=waited,reference=waited;
        uint32_t panel=0;
        assert(waited.access(eim,0x20086a7c,0x16010524,4,false,panel)==A::ok && panel==0x102aa);
        panel |= 0x4001;
        assert(waited.access(eim,0x20086a82,0x16010524,4,true,panel)==A::ok);
        assert(waited.displayState().activeWord(0x524)==0x142ab);
        uint32_t pulse=0;
        assert(waited.access(eim,0x200869c8,0x160104f0,4,false,pulse)==A::ok && pulse==0);
        pulse=0x407f0000;assert(waited.access(eim,0x200869f0,0x160104f0,4,true,pulse)==A::ok);
        pulse|=0x80000000u;assert(waited.access(eim,0x200869f0,0x160104f0,4,true,pulse)==A::sideEffect);
        pulse=0;assert(waited.access(eim,0x200869f2,0x160104f0,4,false,pulse)==A::ok && pulse==0x407f0000);
        partition=waited;reference=waited;
        const auto phase=waited.displayState().scanPhase();
        const Imx1PllRatio bus={128,390625},cpu={64,390625};
        waited.advanceBusCycles(17,bus,cpu);
        for (unsigned i=0;i<17;++i) partition.advanceBusCycles(1,bus,cpu);
        reference.advanceReference(17*bus.numerator,bus.denominator);
        assert(waited.displayState().scanPhase()!=phase);
        assert(waited.displayState().scanPhase()==partition.displayState().scanPhase());
        assert(waited.displayState().scanPhase()==reference.displayState().scanPhase());
        waited.advanceBusCycles(19,bus,{125,762752});
        for (unsigned i=0;i<19;++i) partition.advanceBusCycles(1,bus,{125,762752});
        reference.advanceReference(19*bus.numerator,bus.denominator);
        assert(waited.displayState().scanPhase()==reference.displayState().scanPhase());
        assert(waited.displayState().scanPhase()==partition.displayState().scanPhase());
        assert(waited.displayState().frameNumber()==reference.displayState().frameNumber());
        assert(waited.displayState().verticalBlankEvents()==reference.displayState().verticalBlankEvents());
        assert(waited.displayState().verticalBlankEvents()==partition.displayState().verticalBlankEvents());
        assert(waited.interruptState().pendingValue()==reference.interruptState().pendingValue());
    }


    {
        auto staged = state, reference = state;
        Imx1GraphicsMemory bytes;
        Imx1GraphicsFrame output;
        uint32_t rgb = 0xf800;
        assert(bytes.accessGpu(staged,eim,0x800000,2,true,rgb) == A::ok);
        assert(output.advance(staged,bytes,eim));
        uint32_t first = 99, last = 99;
        assert(output.getPendingFrame(first,last)[0] == 0xff0000ff);
        output.acknowledgeFrame();
        const auto revision = staged.displayState().revision();
        transaction(staged,eim,true);
        store(staged,eim,0x200ad2f8,0x420,0);
        for (auto field : {std::array<uint32_t,3>{0x200ad2f8,0x420,0x80000000},
                {0x200ad304,0x424,0x08000000},{0x200ad310,0x428,0x4000}}) {
            const auto before = staged.displayState().stagedWord(field[1]);
            uint32_t rejected = field[2];
            assert(staged.access(eim,field[0],0x16010000+field[1],4,true,rejected) == A::sideEffect);
            assert(staged.displayState().stagedWord(field[1]) == before);
        }
        staged.advanceCpuCycles(32);
        reference.advanceCpuCycles(32);
        assert(staged.displayState().lcdEnabled() && staged.displayState().clockAcknowledgements() == 0x6000000);
        assert(staged.displayState().activeWord(0x420) == 0x60008 && staged.displayState().revision() == revision);
        assert(staged.displayState().frameNumber() == reference.displayState().frameNumber());
        assert(output.advance(staged,bytes,eim) && !output.getPendingFrame(first,last));
        uint32_t submit = 0x7b;
        assert(staged.access(eim,0x200aeb68,0x160104d8,4,true,submit) == A::sideEffect);
        assert(!staged.displayState().latchPending());
        staged.advanceCpuCycles(32);
        reference.advanceCpuCycles(32);
        assert(staged.displayState().lcdEnabled() && staged.displayState().clockAcknowledgements() == 0x6000000);
        uint32_t phase = 0, referencePhase = 0;
        assert(staged.access(eim,0x200ad7b0,0x1601054c,4,false,phase) == A::ok);
        assert(reference.access(eim,0x200ad7b0,0x1601054c,4,false,referencePhase) == A::ok && phase == referencePhase);
        store(staged,eim,0x200ad2f8,0x420,0x60008);
        store(staged,eim,0x200ad304,0x424,0x50001);
        store(staged,eim,0x200ad310,0x428,0x20001);
        store(staged,eim,0x200ad65c,0x42c,0x50001);
        store(staged,eim,0x200ad668,0x430,0x20001);
        transaction(staged,eim,false);
        uint32_t stopClock = 0x1001c06;
        assert(staged.access(eim,0x200ada9c,0x16010414,4,true,stopClock) == A::sequence);
        staged.advanceCpuCycles(period - 65);
        assert(staged.displayState().latchPending() && staged.displayState().activeWord(0x424) == 0x40001);
        assert(staged.displayState().clockAcknowledgements() == 0x6000000);
        assert(output.advance(staged,bytes,eim) && !output.getPendingFrame(first,last));
        staged.advanceCpuCycles(1);
        assert(!staged.displayState().latchPending() && staged.displayState().activeWord(0x424) == 0x50001);
        assert(staged.displayState().revision() == revision + 1 && staged.displayState().clockAcknowledgements() == 0x6000000);
        assert(output.advance(staged,bytes,eim));
        const auto* image = output.getPendingFrame(first,last);
        assert(image && output.width() == 4 && output.height() == 1 && first == 0 && last == 0 && image[0] == 0xff0000ff);
    }

    value = 0xdeadbeef;
    assert(state.access(eim,0x200ad7b0,0x1601054c,4,false,value) == A::ok && value == 0);
    state.advanceCpuCycles(8 * Imx1GraphicsDisplay::pixelCycles);
    assert(state.access(eim,0x200ad7b0,0x1601054c,4,false,value) == A::ok && value == 0x400);
    {
        // The native two-phase wait observes entry into and exit from the active interval.
        auto waiting = state;
        const auto phase = waiting.displayState().scanPhase();
        for (unsigned i = 0; i < 16; ++i) {
            value = 0;
            assert(waiting.access(eim,0x200adb08,0x1601054c,4,false,value) == A::ok && value == 0x400);
            assert(waiting.access(eim,0x200adb14,0x1601054c,4,false,value) == A::ok && value == 0x400);
        }
        assert(waiting.displayState().scanPhase() == phase);
        unsigned ticks = 0;
        do {
            waiting.advanceCpuCycles(1);
            assert(waiting.access(eim,0x200adb14,0x1601054c,4,false,value) == A::ok);
            assert(++ticks <= period);
        } while (value & 0x400);
        do {
            waiting.advanceCpuCycles(1);
            assert(waiting.access(eim,0x200adb08,0x1601054c,4,false,value) == A::ok);
            assert(++ticks <= 2 * period);
        } while (!(value & 0x400));
        assert(ticks == period);
        value = 0xdeadbeef;
        assert(waiting.access(eim,0x200adb0c,0x1601054c,4,false,value) == A::externalInput && value == 0xdeadbeef);
        for (uint32_t pc : {0x200adb08u,0x200adb14u}) {
            assert(waiting.access(eim,pc,0x1601054c,4,true,value) == A::externalInput);
            assert(waiting.access(eim,pc,0x1601054c,2,false,value) == A::width);
            assert(value == 0xdeadbeef);
        }
        assert(waiting.displayState().scanPhase() == phase);
        auto resetDisplay = waiting.displayState();
        resetDisplay.reset();
        for (uint32_t pc : {0x200adb08u,0x200adb14u}) {
            assert(resetDisplay.access(Imx1GraphicsWindows{},pc,0x1601054c,4,false,value) == A::ok && value == 0);
        }
    }
    // Advancing one large interval and many small ones crosses the same deadlines.
    auto chunked = state;
    state.advanceCpuCycles(3 * period + 7);
    for (unsigned i = 0; i < 3 * period + 7; ++i) chunked.advanceCpuCycles(1);
    assert(state.displayState().frameNumber() == chunked.displayState().frameNumber());
    assert(state.displayState().verticalBlankEvents() == chunked.displayState().verticalBlankEvents());
    assert(state.interruptState().pendingValue() == chunked.interruptState().pendingValue());
    assert(state.displayState().revision() == chunked.displayState().revision());

    Imx1GraphicsMemory memory;
    assert(memory.contains(state,eim,0x100000,0x60000));
    assert(memory.contains(state,eim,0xffffff,1));
    assert(!memory.contains(state,eim,0xffffff,2));
    assert(!memory.contains(state,eim,0x800000,~uint64_t(0)));
    assert(!memory.contains(state,eim,0x80000,8));
    uint8_t unchanged[4] = {1,2,3,4};
    assert(!memory.copyGpuRange(state,eim,0xfffffe,4,unchanged));
    assert(unchanged[0] == 1 && unchanged[3] == 4);
    auto pixel = [&](uint32_t address, uint32_t rgb) {
        assert(memory.accessGpu(state,eim,address,2,true,rgb) == A::ok);
    };
    for (auto pair : {std::array<uint32_t,2>{0x800000,0xf800},{0x800002,0x07e0},{0x800004,0x001f},
            {0x800006,0xffff},{0x800008,0xffff},{0x80000a,0},{0x80000c,0x8410}}) pixel(pair[0],pair[1]);
    auto scanout = memory.dirty(1,Imx1GraphicsMemory::Consumer::scanout);
    memory.clearDirty(1);
    assert(!memory.dirty(1).pending && memory.dirty(1,Imx1GraphicsMemory::Consumer::scanout).pending);
    Imx1GraphicsFrame frame;
    assert(frame.advance(state,memory,eim));
    uint32_t first = 99, last = 99;
    const uint32_t* image = frame.getPendingFrame(first,last);
    assert(image && frame.width() == 3 && frame.height() == 2 && first == 0 && last == 1);
    assert(image[0] == 0xff0000ff && image[1] == 0xff00ff00 && image[2] == 0xffff0000);
    assert(image[3] == 0xffffffff && image[4] == 0xff000000 && image[5] == 0xff848284);
    assert(!memory.dirty(1,Imx1GraphicsMemory::Consumer::scanout).pending);
    assert(memory.dirty(1,Imx1GraphicsMemory::Consumer::upload).pending);
    frame.acknowledgeFrame();
    assert(!frame.getPendingFrame(first,last));
    // Padding and nonvisible bytes do not create visible rows.
    pixel(0x800006,0);
    state.advanceCpuCycles(period);
    assert(frame.advance(state,memory,eim) && !frame.getPendingFrame(first,last));
    pixel(0x801000,0xffff);
    state.advanceCpuCycles(period);
    assert(frame.advance(state,memory,eim) && !frame.getPendingFrame(first,last));
    pixel(0x80000c,0xf800);
    scanout = memory.dirty(1,Imx1GraphicsMemory::Consumer::scanout);
    pixel(0x80000a,0x07e0);
    memory.acknowledgeDirty(1,Imx1GraphicsMemory::Consumer::scanout,scanout.generation);
    assert(memory.dirty(1,Imx1GraphicsMemory::Consumer::scanout).pending);
    // A host frame read does not advance guest time or expose a future frame.
    assert(frame.advance(state,memory,eim) && !frame.getPendingFrame(first,last));
    state.advanceCpuCycles(period);
    assert(frame.advance(state,memory,eim));
    image = frame.getPendingFrame(first,last);
    assert(image && first == 1 && last == 1 && image[4] == 0xff00ff00 && image[5] == 0xff0000ff);
    pixel(0x800000,0x001f);
    state.advanceCpuCycles(period);
    assert(frame.advance(state,memory,eim));
    frame.acknowledgeFrame();
    image = frame.getPendingFrame(first,last);
    assert(image && first == 0 && last == 1 && image[0] == 0xffff0000);
    frame.acknowledgeFrame();
    assert(!frame.getPendingFrame(first,last));
    // Invalid enabled descriptors reject without publishing or disturbing active state.
    transaction(state,eim,true);
    store(state,eim,0x200ad720,0x418,0xfffffc);
    value = 0x7b;
    const auto revision = state.displayState().revision();
    assert(state.access(eim,0x200aeb68,0x160104d8,4,true,value) == A::sideEffect);
    assert(state.displayState().revision() == revision && !state.displayState().latchPending());
    store(state,eim,0x200ad720,0x418,0x800000);
    value = 7;
    assert(state.access(eim,0x200ad738,0x16010414,4,true,value) == A::sideEffect);
    assert(state.displayState().stagedWord(0x414) == 0x1801c06);
    transaction(state,eim,false);
    for (unsigned i = 0; i < 100; ++i) {
        assert(frame.advance(state,memory,eim));
        frame.getPendingFrame(first,last);
        frame.acknowledgeFrame();
        assert(state.displayState().latchPending() && state.displayState().revision() == revision);
    }
    state.advanceCpuCycles(period);
    assert(frame.advance(state,memory,eim) && frame.getPendingFrame(first,last));
    // A resized frame cannot retain row bounds from an older unacknowledged image.
    transaction(state,eim,true);
    store(state,eim,0x200ad304,0x424,0x50001);
    store(state,eim,0x200ad310,0x428,0x20001);
    store(state,eim,0x200ad65c,0x42c,0x50001);
    store(state,eim,0x200ad668,0x430,0x20001);
    transaction(state,eim,false);
    state.advanceCpuCycles(period);
    assert(frame.advance(state,memory,eim));
    image = frame.getPendingFrame(first,last);
    assert(image && frame.width() == 4 && frame.height() == 1 && first == 0 && last == 0);
    frame.acknowledgeFrame();
    {
        auto clocked = state;
        const auto phase = clocked.displayState().scanPhase();
        store(clocked,eim,0x200c5f20,0x194,0x1545);
        assert(clocked.memoryReady(eim) && clocked.displayState().scanPhase()==phase);
        uint32_t aperture=0;
        assert(clocked.access(eim,0x200c1118,0x16000010,1,false,aperture)==A::ok && aperture==1);
        assert(clocked.access(eim,0x200c111c,0x16000010,1,true,aperture)==A::ok);
        aperture=2;
        assert(clocked.access(eim,0x200c111c,0x16000010,1,true,aperture)==A::sideEffect);
        store(clocked,eim,0x200c45e4,0x80,0x3f);
        store(clocked,eim,0x200c4920,0x90,0x20);
        assert(clocked.displayState().scanPhase() == phase && clocked.displayState().lcdEnabled());
        auto split = clocked;
        clocked.advanceCpuCycles(1001,{1,3});
        for (unsigned i=0;i<1001;++i) split.advanceCpuCycles(1,{1,3});
        clocked.advanceCpuCycles(2001,{1,7});
        for (unsigned i=0;i<2001;++i) split.advanceCpuCycles(1,{1,7});
        assert(clocked.displayState().scanPhase() == split.displayState().scanPhase());
        assert(clocked.displayState().frameNumber() == split.displayState().frameNumber());
        // Equal elapsed reference time under a changed CPU rate has equal phase.
        auto faster = split;
        clocked.advanceCpuCycles(1000,{1,3});
        faster.advanceCpuCycles(2000,{1,6});
        assert(clocked.displayState().scanPhase() == faster.displayState().scanPhase());
        assert(clocked.displayState().frameNumber() == faster.displayState().frameNumber());
        const auto held = clocked.displayState().scanPhase();
        store(clocked,eim,0x200c5ca8,0x88,0x4b000201);
        store(clocked,eim,0x200c47b0,0x8c,0x301);
        assert(!clocked.displayState().lcdEnabled());
        clocked.advanceCpuCycles(1000);
        assert(clocked.displayState().scanPhase() == held);
        store(clocked,eim,0x200c55c0,0x8c,0x300);
        assert(clocked.displayState().lcdEnabled() && clocked.displayState().scanPhase() == held);
        transaction(clocked,eim,true);
        store(clocked,eim,0x200ad720,0x418,0x800008);
        transaction(clocked,eim,false);
        auto pending = clocked;
        store(clocked,eim,0x200c4920,0x90,0x10);
        store(pending,eim,0x200c4920,0x90,0x10);
        clocked.advanceCpuCycles(2000);
        for (unsigned i=0;i<2000;++i) pending.advanceCpuCycles(1);
        assert(!clocked.displayState().latchPending() && !pending.displayState().latchPending());
        assert(clocked.displayState().activeWord(0x418) == 0x800008);
        assert(clocked.displayState().scanPhase() == pending.displayState().scanPhase());
        assert(clocked.displayState().frameNumber() == pending.displayState().frameNumber());
        store(clocked,eim,0x200ada9c,0x414,0x1001c06);
        store(clocked,eim,0x200ad804,0x414,0x1801c06);
        clocked.advanceCpuCycles(31);
        assert(!(clocked.displayState().clockAcknowledgements()&0x2000000) && !clocked.displayState().lcdEnabled());
        clocked.advanceCpuCycles(1);
        assert(clocked.displayState().clockAcknowledgements()==0x6000000 && clocked.displayState().lcdEnabled());
    }
    {
        auto rejected=state;
        store(rejected,eim,0x200c45e4,0x80,0x3f);
        store(rejected,eim,0x200c47b0,0x8c,0x300);
        store(rejected,eim,0x200c4920,0x90,0x20);
        store(rejected,eim,0x200c49ac,0x84,0x25120b01);
        store(rejected,eim,0x200c4b38,0x88,0x4b001200);
        store(rejected,eim,0x200c4c00,0x98,0x00ff4004);
        store(rejected,eim,0x200c4cac,0x9c,0x6e);
        store(rejected,eim,0x200c4cdc,0xa0,0xffff0000);
        store(rejected,eim,0x200c5824,0x94,0x203);
        store(rejected,eim,0x200c5850,0x94,3);
        store(rejected,eim,0x200c587c,0x94,0x103);
        rejected.advanceCpuCycles(300);
        transaction(rejected,eim,true);
        store(rejected,eim,0x200ad720,0x418,0x800008);
        transaction(rejected,eim,false);
        assert(rejected.displayState().latchPending() && rejected.clockState().count()>0 && rejected.clockState().nextEvent()>0);
        const auto reference=rejected;
        for (auto op : {std::array<uint32_t,3>{0x200c47b0,0x8c,0x80000301},
                {0x200c47b0,0x8c,0x8300},{0x200c4920,0x90,0x10020},
                {0x200c4b38,0x88,0x4b011200},{0x200c4b38,0x88,0x4b001280},
                {0x200c4c00,0x98,0x00ff4006},{0x200c4c00,0x98,0x00ff4014},
                {0x200c4c00,0x98,0x00ff4104},{0x200c4cac,0x9c,0x8000006e},
                {0x200c4cac,0x9c,0x106e},{0x200c4cdc,0xa0,0xffff0001},
                {0x200c4cdc,0xa0,0x7fff0000}}) {
            auto bad=op[2];
            assert(rejected.access(eim,op[0],0x16010000+op[1],4,true,bad)==A::sideEffect && bad==op[2]);
            for (uint32_t off=0x80;off<=0xb0;off+=4)
                assert(rejected.clockState().word(off)==reference.clockState().word(off));
            assert(rejected.clockState().count()==reference.clockState().count());
            assert(rejected.clockState().nextEvent()==reference.clockState().nextEvent());
            for (auto rate : {std::array<Imx1GraphicsClocks::Rate,2>{rejected.clockState().pllRate(),reference.clockState().pllRate()},
                    {rejected.clockState().systemRate(),reference.clockState().systemRate()},
                    {rejected.clockState().pixelRate(),reference.clockState().pixelRate()}})
                assert(rate[0].numerator==rate[1].numerator && rate[0].denominator==rate[1].denominator);
            assert(rejected.displayState().scanPhase()==reference.displayState().scanPhase());
            assert(rejected.displayState().frameNumber()==reference.displayState().frameNumber());
            assert(rejected.displayState().revision()==reference.displayState().revision());
            assert(rejected.displayState().clockAcknowledgements()==reference.displayState().clockAcknowledgements());
            assert(rejected.displayState().latchPending());
        }
        auto future=reference;
        rejected.advanceCpuCycles(2000);future.advanceCpuCycles(2000);
        assert(rejected.clockState().count()==future.clockState().count() && rejected.clockState().nextEvent()==future.clockState().nextEvent());
        assert(rejected.displayState().scanPhase()==future.displayState().scanPhase() && rejected.displayState().revision()==future.displayState().revision());
    }
    // Clock requests really gate acknowledgements and output.
    store(state,eim,0x200ada9c,0x414,0x1001c06);
    value = 0;
    assert(state.access(eim,0x200adaa4,0x16010414,4,false,value) == A::ok && !(value & 0x2000000));
    assert(!state.displayState().lcdEnabled());
    assert(frame.advance(state,memory,eim) && !frame.lcdEnabled());
    state.reset();
    frame.reset();
    assert(!state.displayState().latchPending() && !state.displayState().lcdEnabled());
    assert(state.displayState().clockAcknowledgements() == 0 && memory.dirty(1,Imx1GraphicsMemory::Consumer::upload).pending);
    configureGraphicsInterface(state,eim);
    configureGraphicsController(state,eim);
    engine(state,eim);
    mode(state,eim,0x100000);
    assert(frame.advance(state,memory,eim) && frame.getPendingFrame(first,last));
    assert(first == 0 && last == 1);
    std::puts("PASS: staged display/latch clocks, RGB565 frame, stride/bounds, independent generations and reset");
}
