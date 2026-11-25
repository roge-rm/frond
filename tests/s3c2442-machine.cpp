// S3C2442 machine checks on the CPU: idle ends or stops, the budget bounds idle time, and start() resets
// the undefined instruction stop.

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CPU.h"
#include "rx1950_board.h"
#include "rx1950_power.h"
#include "s3c2442_machine.h"
#include "s3c2442_pen.h"
#include "vSD.h"

namespace {
    constexpr uint32_t code = 0x30000000;
    constexpr uint32_t wfi = 0xee070f90, loop = 0xeafffffe, undefined = 0xe7f000f0;

    // A machine with the watchdog off (its reset value runs it with the reset on), every interrupt
    // masked and a branch to itself on each vector.
    std::unique_ptr<S3c2442Machine> make(const std::vector<uint32_t>& program, const S3c2442Options& options = {}) {
        std::string error;
        auto m = S3c2442Machine::create(options, error);
        assert(m);
        m->watchdog.wtcon = 0;
        for (uint32_t v = 0; v < 0x20; v += 4) std::memcpy(m->steppingstone + v, &loop, 4);
        assert(m->loadRam(code, reinterpret_cast<const uint8_t*>(program.data()), program.size() * 4));
        m->start(code);
        return m;
    }

    unsigned mode(S3c2442Machine& m) { return cpuGetRegExternal(m.cpu(), ARM_REG_NUM_CPSR) & 0x1f; }

    constexpr uint32_t mcr(unsigned crn, unsigned rd) { return 0xee000f10u | crn << 16 | rd << 12; }

    // A program that stores each value at its address (ldr r0, ldr r1 from a literal pool, str r1, [r0]),
    // then runs tail.
    std::vector<uint32_t> stores(const std::vector<std::pair<uint32_t, uint32_t>>& writes, const std::vector<uint32_t>& tail) {
        std::vector<uint32_t> out;
        const uint32_t pool = uint32_t(writes.size() * 3 + tail.size());
        for (size_t i = 0; i < writes.size(); ++i) {
            for (unsigned r = 0; r < 2; ++r) {
                const uint32_t at = uint32_t(out.size()), literal = pool + uint32_t(i * 2 + r);
                out.push_back(0xe59f0000u | r << 12 | ((literal - at - 2) * 4));
            }
            out.push_back(0xe5801000);  // str r1, [r0]
        }
        out.insert(out.end(), tail.begin(), tail.end());
        for (const auto& w : writes) {
            out.push_back(w.first);
            out.push_back(w.second);
        }
        return out;
    }

    struct Frame { int16_t left, right; uint32_t gain; };
    bool operator==(const Frame& a, const Frame& b) { return a.left == b.left && a.right == b.right && a.gain == b.gain; }

    // Playback as wavedev.dll sets it up: DMA channel 2 from a 32-frame SDRAM buffer (left n, right -n) to
    // IISFIFO with auto reload and its interrupt, the IIS on MPLLin at 384fs transmitting, the codec playing
    // and the speaker amplifier (GPA1) on. Then IRQs on and a wait for interrupt.
    std::unique_ptr<S3c2442Machine> playback(std::vector<Frame>& frames, bool fast) {
        constexpr uint32_t buffer = 0x30010000;
        auto m = make(stores({{0x56000000, 0x7ffffd}, {0x56000004, 2},          // GPA1 an output, high
                              {0x4b000080, buffer}, {0x4b000084, 0}, {0x4b000088, 0x55000010}, {0x4b00008c, 3},
                              {0x4b000090, 0xa0900000 | 64},
                              {0x55000004, 0x28d}, {0x55000008, 0x42}, {0x5500000c, 0xa000}, {0x55000000, 0x23},
                              {0x4b0000a0, 2}, {0x4a000008, ~(1u << 19)}},
                             {0xe321f013, wfi, loop}));
        if (fast && !m->setFastEngine(true)) return nullptr;
        for (uint16_t n = 0; n < 32; ++n) {
            const int16_t frame[2] = {int16_t(n), int16_t(-n)};
            assert(m->loadRam(buffer + n * 4, reinterpret_cast<const uint8_t*>(frame), 4));
        }
        m->codec.set(0x02, 0x400);
        m->codec.set(0x13, 0);
        m->pcmSink = [&frames](int16_t l, int16_t r, uint32_t gain) { frames.push_back({l, r, gain}); };
        return m;
    }

    void pwmTimer4(S3c2442Machine& m, uint32_t buffer) {
        m.pwm.write(0x3c, buffer);                // TCNTB4
        m.pwm.write(0x08, (1u << 21) | (1u << 22));  // manual update, auto reload
        m.pwm.write(0x08, (1u << 20) | (1u << 22));  // start, auto reload
    }
}

namespace {
    // A 32 KB card in RAM, sector n filled with n and its offset.
    std::vector<uint8_t> disk;
    bool diskRead(uint32_t sector, void* buf) {
        if ((sector + 1) * 512 > disk.size()) return false;
        std::memcpy(buf, disk.data() + sector * 512, 512);
        return true;
    }
    bool diskWrite(uint32_t sector, const void* buf) {
        if ((sector + 1) * 512 > disk.size()) return false;
        std::memcpy(disk.data() + sector * 512, buf, 512);
        return true;
    }
    // CMD0, ACMD41 until ready, CMD2, CMD3 (RCA 3), CMD7 and CMD16 512: the card in transfer state.
    VSD* readyCard() {
        disk.assign(64 * 512, 0);
        for (size_t i = 0; i < disk.size(); ++i) disk[i] = uint8_t(i / 512 * 16 + i % 16);
        VSD* card = vsdInit(diskRead, diskWrite, 64);
        uint8_t reply[17];
        vsdCommand(card, 0, 0, reply);
        for (unsigned i = 0; i < 100; ++i) {
            vsdCommand(card, 55, 0, reply);
            vsdCommand(card, 41, 0x00ff8000, reply);
            if (reply[1] & 0x80) break;
        }
        assert(reply[1] & 0x80);
        assert(vsdCommand(card, 2, 0, reply) == SdReply136bits);
        assert(vsdCommand(card, 3, 0, reply) == SdReply48bits);
        assert(vsdCommand(card, 7, 3u << 16, reply) == SdReply48bitsAndBusy);
        assert(vsdCommand(card, 16, 512, reply) == SdReply48bits);
        return card;
    }
}

// The SD slot through the guest: a DMA read of sector 1 on channel 0 as SDHC_SC2410.dll sets it up (SDIDAT to
// SDRAM in words, 0x8ae000), a DMA write of SDRAM to sector 2 (0x92f000), DatFin and the DMA interrupt, the
// card's supply off on GPJ1, and the card section of a saved state.
static void sdTests() {
    constexpr uint32_t buffer = 0x3004c000;
    VSD* card = readyCard();
    const std::vector<std::pair<uint32_t, uint32_t>> readSector = {
        {0x5a000038, 0x10000}, {0x5a000034, 0xffffffff}, {0x5a000024, 0x7fffff}, {0x5a000028, 512},
        {0x4b000000, 0x5a000040}, {0x4b000004, 3}, {0x4b000008, buffer}, {0x4b00000c, 0},
        {0x4b000010, 0xa2e00000 | 128}, {0x5a00002c, 0x8ae000 | 1}, {0x5a000008, 512}, {0x5a00000c, 0x351},
        {0x4b000020, 2}};
    auto m = make(stores(readSector, {loop}));
    m->setSdCard(card);
    assert(m->sdCard() == card && m->sdPowered());
    assert(m->run(10000) == S3c2442Stop::budget);
    assert(std::memcmp(m->sdram() + (buffer - 0x30000000), disk.data() + 512, 512) == 0);
    assert((m->intc.pending() & (1u << s3cIrq::dma0)) && !m->dma.on(0) && m->sdi.blocksRead == 1);
    uint32_t v = 0;
    assert(m->sdi.read(0x34, 4, v) == S3cAccess::ok && (v & 0x13) == 0x10);

    // SDRAM to sector 2 after CMD24.
    std::vector<uint8_t> data(512);
    for (unsigned i = 0; i < 512; ++i) data[i] = uint8_t(0xa5 ^ i);
    const std::vector<std::pair<uint32_t, uint32_t>> writeSector = {
        {0x5a00002c, 0}, {0x5a000038, 0x10000}, {0x5a000034, 0xffffffff}, {0x5a000028, 512},
        {0x4b000000, buffer}, {0x4b000004, 0}, {0x4b000008, 0x5a000040}, {0x4b00000c, 3},
        {0x4b000010, 0xa2e00000 | 128}, {0x5a00002c, 0x92f000 | 1}, {0x5a000008, 1024}, {0x5a00000c, 0x358},
        {0x4b000020, 2}};
    auto w = make(stores(writeSector, {loop}));
    assert(w->loadRam(buffer, data.data(), data.size()));
    w->setSdCard(card);
    assert(w->run(10000) == S3c2442Stop::budget);
    assert(std::memcmp(disk.data() + 1024, data.data(), 512) == 0 && w->sdi.blocksWritten == 1);
    assert((w->intc.pending() & (1u << s3cIrq::dma0)) && w->sdi.read(0x34, 4, v) == S3cAccess::ok && (v & 0x13) == 0x10);

    // The saved state keeps the card in, its id and its state; a state's loadedCard says so.
    w->sdCardId = "card-1";
    std::vector<uint8_t> saved;
    std::string why;
    assert(w->saveState(saved, why));
    auto n = make({loop});
    assert(n->loadState(saved.data(), saved.size(), why));
    assert(n->loadedCard.section && n->loadedCard.in && n->loadedCard.id == "card-1" && !n->loadedCard.card.empty());
    assert(n->sdi.blocksWritten == 1 && !n->sdCard());
    // Saved with the slot empty: no card in.
    w->setSdCard(nullptr);
    assert(w->saveState(saved, why) && n->loadState(saved.data(), saved.size(), why));
    assert(n->loadedCard.section && !n->loadedCard.in);

    // GPJ1 an output driven low: the card loses its supply, resets and stops answering; CMD13 times out.
    const std::vector<std::pair<uint32_t, uint32_t>> powerOff = {
        {0x560000d4, 0}, {0x560000d0, 1u << 2}, {0x5a000010, 0x1e00}, {0x5a000008, 3u << 16}, {0x5a00000c, 0x34d}};
    auto p = make(stores(powerOff, {loop}));
    p->setSdCard(card);
    assert(p->run(10000) == S3c2442Stop::budget && !p->sdPowered());
    assert(p->sdi.read(0x10, 4, v) == S3cAccess::ok && (v & 0x600) == 0x400);
    // Back to idle, so a read command doesn't start a transfer.
    uint8_t reply[17], sector[512];
    vsdCommand(card, 17, 0, reply);
    assert(vsdDataXferBlockFromCard(card, sector, 512) != SdDataOk);
    p->setSdCard(nullptr);
    free(card);

    // GPJ1 dropped mid-read: DatTout reaches INT_SDI through SDIIMSK straight away.
    card = readyCard();
    const std::vector<std::pair<uint32_t, uint32_t>> dropMidRead = {
        {0x5a00003c, 1u << 8}, {0x5a000028, 512}, {0x5a00002c, 0xa2000 | 1}, {0x5a000008, 512}, {0x5a00000c, 0x351},
        {0x560000d4, 0}, {0x560000d0, 1u << 2}};
    auto q = make(stores(dropMidRead, {loop}));
    q->setSdCard(card);
    assert(q->run(10000) == S3c2442Stop::budget && !q->sdPowered() && q->sdi.blocksRead == 1);
    assert(q->sdi.read(0x34, 4, v) == S3cAccess::ok && (v & 0x21) == 0x20);
    assert(q->intc.srcpnd & (1u << s3cIrq::sdi));
    q->setSdCard(nullptr);
    free(card);
}

// Sound: IIS playback through DMA, its interrupt and timing in both engines, the board's gain, recording from
// the microphone into SDRAM, the codec's reset pin, headphones, and a save and load mid-stream.
static void audioTests() {
    // Playback. Turning the channel on fills the 32-entry FIFO at once, so its count is 32 of 64; each
    // 44100 Hz frame then takes two halfwords, so the interrupt comes after 16 frames and the channel reloads.
    std::vector<Frame> exact;
    std::vector<uint64_t> irqNs;
    for (bool fast : {false, true}) {
        std::vector<Frame> frames;
        auto m = playback(frames, fast);
        if (!m) continue;
        m->run(60);
        assert(m->iis.transmitting() && m->dma.remaining(2) == 32 && frames.empty());
        uint64_t rn = 0, rd = 0;
        m->audioRate(rn, rd);
        assert(rn == 16934400 && rd == 384);
        // Until the IRQ: the CPU leaves its wait in IRQ mode.
        while (mode(*m) != 0x12) m->run(1000);
        assert(frames.size() == 16 && (m->intc.srcpnd & (1u << 19)) && m->dma.remaining(2) == 64);
        irqNs.push_back(m->nanoseconds());
        // 0.1 s of guest time: 4410 frames, the buffer over and over, nothing short.
        const uint64_t until = m->nanoseconds() + 100000000;
        while (m->nanoseconds() < until) m->run(100000);
        const uint64_t expected = m->nanoseconds() * 44100 / 1000000000;
        assert(frames.size() + 1 >= expected && frames.size() <= expected + 1 && m->iis.underruns == 0);
        for (size_t i = 0; i < frames.size(); ++i)
            assert(frames[i].left == int16_t(i % 32) && frames[i].right == -int16_t(i % 32) && frames[i].gain == 65536);
        if (!fast) exact = frames;
        else {
            const size_t n = std::min(exact.size(), frames.size());
            assert(std::equal(frames.begin(), frames.begin() + n, exact.begin()));
            assert(irqNs[0] == irqNs[1]);
        }
    }
    // The board's gain: the amplifier off, then headphones in with the codec's headphone driver.
    {
        std::vector<Frame> frames;
        auto m = playback(frames, false);
        m->run(1000);
        assert(m->audioGain() == 65536);
        m->gpio.dat[S3cGpio::a] &= ~2u;
        assert(m->audioGain() == 0 && !m->headphonesIn());
        m->setHeadphones(true);
        assert(m->headphonesIn() && !m->gpio.pin(S3cGpio::g, 12) && m->audioGain() == 0);
        m->codec.set(0x02, 0x2400);
        assert(m->audioGain() == 65536);
        m->codec.set(0x13, 0x4000);  // master mute
        assert(m->audioGain() == 0);
        m->codec.set(0x13, 0);
        // GPJ0 driven low powers the codec off.
        m->gpio.con[S3cGpio::j] = (m->gpio.con[S3cGpio::j] & ~3u) | 1;
        m->gpio.dat[S3cGpio::j] &= ~1u;
        assert(m->audioGain() == 0);
    }
    // A save mid-stream (FIFO part full, DMA part way) and a load into another machine play on the same.
    {
        std::vector<Frame> a, b;
        auto m = playback(a, false);
        m->run(3000000);
        std::vector<uint8_t> state;
        std::string why;
        assert(m->saveState(state, why));
        auto n = playback(b, false);
        assert(n->loadState(state.data(), state.size(), why));
        const size_t before = a.size();
        m->run(5000000);
        n->run(5000000);
        assert(a.size() > before + 100 && b.size() == a.size() - before);
        assert(std::equal(b.begin(), b.end(), a.begin() + before));
    }
    // Recording: DMA channel 1 from IISFIFO into SDRAM for one 32-frame run, the codec's ADC on, the microphone a counter. The
    // guest pulses the codec's reset (GPD0 high) first, which clears the registers the host set.
    {
        constexpr uint32_t buffer = 0x30020000;
        auto m = make(stores({{0x56000030, 1}, {0x56000034, 1}, {0x56000034, 0},
                              {0x4b000040, 0x55000010}, {0x4b000044, 3}, {0x4b000048, buffer}, {0x4b00004c, 0},
                              {0x4b000050, 0xa2d00000 | 64},  // RELOAD: off after one run
                              {0x55000004, 0x24d}, {0x5500000c, 0x5000}, {0x55000000, 0x13},
                              {0x4b000060, 2}},
                             {loop}));
        m->codec.set(0x02, 0x404);
        int16_t counter = 0;
        AudioInputSource mic;
        mic.context = &counter;
        mic.pull = [](void* c, AudioInputRoute route, uint32_t n, uint32_t d) {
            assert(route == AudioInputRoute::internalMic && n == 16934400 && d == 384);
            return ++*static_cast<int16_t*>(c);
        };
        m->microphone = &mic;
        m->run(40);
        assert(m->codec.reg(0x02) == 0 && !m->microphoneDemand().active && m->iis.framesReceived == 0);
        m->codec.set(0x02, 0x404);
        const auto demand = m->microphoneDemand();
        assert(demand.active && demand.rateNumerator == 16934400 && demand.rateDenominator == 384);
        const uint64_t until = m->nanoseconds() + 1000000;
        while (m->nanoseconds() < until) m->run(10000);
        assert(counter >= 40 && !m->dma.on(1));
        std::vector<int16_t> got(64);
        std::memcpy(got.data(), m->sdram() + (buffer - code), 128);
        for (int i = 0; i < 32; ++i) assert(got[i * 2] == i + 1 && got[i * 2 + 1] == i + 1);
        assert(m->intc.srcpnd & (1u << 18));
    }
    // The host microphone: a fence reaches the source once for nested fences and shows in the demand, nothing is
    // pulled while it holds, and a disarm closes the source and lets go of it.
    {
        auto m = make({loop});
        struct Log { int fences = 0, unfences = 0, disarms = 0, pulls = 0; } log;
        AudioInputSource mic;
        mic.context = &log;
        mic.pull = [](void* c, AudioInputRoute, uint32_t, uint32_t) { ++static_cast<Log*>(c)->pulls; return int16_t(1); };
        mic.fence = [](void* c, bool on) { ++(on ? static_cast<Log*>(c)->fences : static_cast<Log*>(c)->unfences); };
        mic.disarm = [](void* c) { ++static_cast<Log*>(c)->disarms; };
        m->microphone = &mic;
        assert(!m->microphoneDemand().fenced);
        m->fenceMicrophone(true);
        m->fenceMicrophone(true);
        assert(m->microphoneDemand().fenced && log.fences == 1);
        m->fenceMicrophone(false);
        assert(m->microphoneDemand().fenced && log.unfences == 0);
        m->fenceMicrophone(false);
        assert(!m->microphoneDemand().fenced && log.unfences == 1);
        m->disarmMicrophone();
        assert(log.disarms == 1 && !m->microphone);
        m->disarmMicrophone();
        assert(log.disarms == 1);
    }
}

// The Wi-Fi chip on nGCS4: the bus reads all ones until GPC8 and GPC9 power it, then the chip ID comes back
// through the indirect register pair; its interrupt pulls GPG8 low.
static void wifiTests() {
    const std::vector<uint32_t> readId = {0xe5902004, loop};  // ldr r2, [r0, #4] after the last store, then loop
    {
        auto m = make(stores({{0x20000004, 0x900}}, readId));
        assert(!m->wifiLive());
        assert(m->run(10000) == S3c2442Stop::budget);
        assert(cpuGetRegExternal(m->cpu(), 2) == 0xffffffffu);
    }
    {
        // A byte read of the chip ID register (0x04) powered gives its byte lane, not 0xff.
        const std::vector<uint32_t> readByte = {0xe5d02005, loop};  // ldrb r2, [r0, #5]
        auto m = make(stores({{0x56000020, 0x50000}, {0x56000024, 0x300}, {0x20000004, 0x900}}, readByte));
        assert(m->run(10000) == S3c2442Stop::budget);
        assert(m->wifiLive());
        assert(cpuGetRegExternal(m->cpu(), 2) == ((Acx100::chipId >> 8) & 0xff));
    }
    {
        auto m = make(stores({{0x56000020, 0x50000}, {0x56000024, 0x300}, {0x20000004, 0x900}}, readId));
        assert(m->run(10000) == S3c2442Stop::budget);
        assert(m->wifiLive());
        assert(cpuGetRegExternal(m->cpu(), 2) == Acx100::chipId);
        assert(m->gpio.input[S3cGpio::g] & (1u << 8));
        // Unmask the started interrupt and let the firmware start: GPG8 goes low.
        m->wifi.write(0x14, 4, 0, 0);
        m->wifi.write(0x18, 4, 0x12345678, 0);
        m->wifi.write(0x04, 4, 0x98, 0);
        m->wifi.write(0x08, 4, ~Acx100::irqStarted, 0);
        m->wifi.write(0x04, 4, 0x2d8, 0);
        m->wifi.write(0x08, 4, 0, m->nanoseconds());
        m->wifiPinsChanged();
        assert(m->run(5000000) == S3c2442Stop::budget);
        assert(m->wifi.running() && !(m->gpio.input[S3cGpio::g] & (1u << 8)));
        // Power off: the chip resets and the line goes back up.
        m->gpio.dat[S3cGpio::c] &= ~0x100u;
        m->wifiPinsChanged();
        assert(!m->wifiLive() && !m->wifi.running() && (m->gpio.input[S3cGpio::g] & (1u << 8)));
    }
}

// USB device: VBUS on GPG5 (low with the cable), the host sees the device once GPJDAT bit 5 (the D+ pull-up) and
// CLKCON's USB device clock are on, a host SETUP raises INT_USBD for the CPU, the endpoint state is saved in its own
// section and an older state without it loads with the endpoints reset; host work due keeps an idle CPU's clock going.
static void usbTests() {
    using R = UsbPacketResult;
    {
        auto m = make(stores({{0x560000d4, 0x20}}, {loop}));
        m->setUsbVbus(true);
        assert(!(m->gpio.input[S3cGpio::g] & (1u << 5)) && m->usbVbus());
        m->clock.clkcon &= ~0x80u;
        assert(m->run(1000) == S3c2442Stop::budget);
        assert(!m->usbListening());
        assert(m->clock.write(0x0c, m->clock.clkcon | 0x80) == S3cAccess::ok);
        assert(m->usbListening());
        m->setUsbVbus(false);
        assert(!m->usbListening() && (m->gpio.input[S3cGpio::g] & (1u << 5)));
    }
    {
        // INT_USBD unmasked, IRQs on, wait for interrupt: a host SETUP wakes it into IRQ mode.
        auto m = make(stores({{0x560000d4, 0x20}, {0x4a000008, ~(1u << s3cIrq::usbd)}}, {0xe321f013, wfi, loop}));
        m->clock.clkcon |= 0x80;
        m->setUsbVbus(true);
        m->hostService = [&] {};
        m->hostNextNs = 10000000000ull;  // host work due later keeps the waiting CPU's clock going
        assert(m->run(1000) == S3c2442Stop::budget && m->usbListening() && mode(*m) == 0x13);
        m->usbDevice.write(0x16c, 0);  // no reset interrupt
        m->usbDevice.busReset();
        m->usbHostChanged();
        assert(!(m->intc.srcpnd & (1u << s3cIrq::usbd)));
        const uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
        assert(m->usbDevice.setup(setup) == R::Ack);
        m->usbHostChanged();
        assert(m->intc.srcpnd & (1u << s3cIrq::usbd));
        const auto woke = m->run(1000);
        assert(woke == S3c2442Stop::budget && mode(*m) == 0x12);

        // The pending SETUP survives a save and load.
        std::vector<uint8_t> state;
        std::string why;
        assert(m->saveState(state, why));
        auto other = make({loop});
        assert(other->loadState(state.data(), state.size(), why));
        assert(other->usbDevice.ep0Csr == 1 && other->usbDevice.fifo[0].size() == 8 && other->usbDevice.controlOpen);
    }
    {
        // Host work due keeps the idle clock going instead of stopping with nothing to wake it, and runs once due.
        auto m = make({wfi, loop});
        uint64_t at = 0;
        m->hostService = [&] { at = m->nanoseconds(); };
        m->hostNextNs = 2000000;
        assert(m->run(1000000000ull) == S3c2442Stop::idleNoWake && at >= 2000000 && at < 2001000);
        assert(m->hostNextNs == UINT64_MAX);
    }
}

// Host work lands at the same guest time and instruction whatever the run slices: on a busy CPU and an idle one, run
// in one go or in uneven slices, on Exact and on Fast.
static void hostServiceTests() {
    for (const bool fast : {false, true})
    for (const bool idle : {false, true}) {
        // A busy loop (subs r0, r0, #1; b back) or a wait for interrupt.
        const std::vector<uint32_t> program = idle ? std::vector<uint32_t>{wfi, loop} : std::vector<uint32_t>{0xe2500001, 0xeafffffd};
        std::vector<std::pair<uint64_t, uint64_t>> seen[2];
        for (unsigned way = 0; way < 2; ++way) {
            auto m = make(program);
            if (fast) assert(m->setFastEngine(true));
            auto& log = seen[way];
            m->hostService = [&] {
                log.emplace_back(m->nanoseconds(), m->retired());
                m->hostNextNs = m->nanoseconds() + 333333;
            };
            m->hostNextNs = 100000;
            const uint64_t total = 3000000;
            if (way == 0) {
                assert(m->run(total) == S3c2442Stop::budget);
            } else {
                uint64_t done = 0, step = 777;
                while (done < total) {
                    const uint64_t n = std::min(step, total - done);
                    assert(m->run(n) == S3c2442Stop::budget);
                    done += n;
                    step = step * 7 % 50021 + 1;
                }
            }
        }
        assert(seen[0].size() > 3 && seen[0] == seen[1]);
        assert(seen[0][0].first >= 100000 && seen[0][0].first < 101000);
        static std::vector<std::pair<uint64_t, uint64_t>> exact[2];
        if (!fast) exact[idle] = seen[0];
        else assert(seen[0] == exact[idle]);
    }
}

// Asleep, host work still runs at its guest times, the same in one run and in uneven slices, and doesn't wake it.
static void hostServiceSleepTests() {
    // mov r0, #0x4c000000; mov r1, #8; str r1, [r0, #0xc] (CLKCON POWER_OFF); b .
    const std::vector<uint32_t> program{0xe3a0044c, 0xe3a01008, 0xe580100c, loop};
    std::vector<std::pair<uint64_t, uint64_t>> seen[2];
    for (unsigned way = 0; way < 2; ++way) {
        auto m = make(program);
        assert(m->run(1000) == S3c2442Stop::budget && m->asleep());
        const uint64_t start = m->nanoseconds();
        auto& log = seen[way];
        m->hostService = [&] {
            assert(m->asleep());
            log.emplace_back(m->nanoseconds(), m->idleCycles());
            m->hostNextNs = m->nanoseconds() + 1000000;
        };
        m->hostNextNs = start + 500000;
        const uint64_t total = 30000000;
        if (way == 0) {
            assert(m->run(total) == S3c2442Stop::budget);
        } else {
            for (uint64_t done = 0, step = 4321; done < total; done += step, step = step * 7 % 900001 + 1)
                assert(m->run(std::min(step, total - done)) == S3c2442Stop::budget);
        }
        assert(m->asleep() && m->wakeCount() == 0);
        assert(log.size() > 10 && log[0].first >= start + 500000 && log[0].first < start + 501000);
    }
    assert(seen[0] == seen[1]);
}

// The host schedule goes with a saved state while it's set, and a state saved without one loads with none due.
static void hostScheduleStateTests() {
    std::vector<uint8_t> state;
    std::string why;
    auto a = make({loop});
    assert(a->saveState(state, why));
    auto b = make({loop});
    b->hostNextNs = 5;
    b->hostUartNextNs[2] = 7;
    b->hostNetNextNs = 9;
    assert(b->loadState(state.data(), state.size(), why));
    assert(b->hostNextNs == UINT64_MAX && b->hostUartNextNs[2] == UINT64_MAX && b->hostNetNextNs == UINT64_MAX);
    a->hostNextNs = 1234567;
    a->hostUartNextNs[0] = 2000000;
    assert(a->saveState(state, why));
    auto c = make({loop});
    assert(c->loadState(state.data(), state.size(), why));
    assert(c->hostNextNs == 1234567 && c->hostUartNextNs[0] == 2000000 && c->hostUartNextNs[1] == UINT64_MAX);
    assert(c->hostNetNextNs == UINT64_MAX);
    // The network poll alone is enough for the section.
    auto d = make({loop});
    d->hostNetNextNs = 3000000;
    assert(d->saveState(state, why));
    auto e = make({loop});
    assert(e->loadState(state.data(), state.size(), why));
    assert(e->hostNetNextNs == 3000000 && e->hostNextNs == UINT64_MAX);
}

// A watchdog reset runs the board's hook at the instruction it happens, whatever the run slices.
static void watchdogHookTests() {
    // WTCNT 200, then WTCON: on, reset on, interrupt off, prescaler 0, clock /16.
    const auto program = stores({{0x53000008, 200}, {0x53000000, 0x0021}}, {loop});
    std::pair<uint64_t, uint64_t> at[2];
    for (unsigned way = 0; way < 2; ++way) {
        auto m = make(program);
        unsigned hooks = 0;
        m->watchdogResetHook = [&] { at[way] = {m->nanoseconds(), m->retired()}; ++hooks; };
        if (way == 0) {
            assert(m->run(200000) == S3c2442Stop::budget);
        } else {
            for (uint64_t done = 0, step = 333; done < 200000; done += step, step = step * 5 % 4099 + 1)
                assert(m->run(std::min(step, 200000 - done)) == S3c2442Stop::budget);
        }
        assert(hooks >= 1 && m->watchdogResets() == hooks);
    }
    assert(at[0] == at[1]);
}

// A power-on reset after a host hard reset: SDRAM cleared, GSTATUS2 PWRST, the RTC's time kept.
static void powerOnResetTests() {
    auto m = make({loop});
    m->rtc.bcd[0] = 0x42;
    m->gpio.gstatus[2] = 0;
    assert(m->run(1000) == S3c2442Stop::budget);
    assert(m->sdram()[code - 0x30000000] != 0);
    m->powerOnReset();
    const uint8_t* ram = m->sdram();
    assert(std::all_of(ram, ram + (1u << 20), [](uint8_t b) { return b == 0; }));
    assert(m->gpio.gstatus[2] == 1 && m->rtc.bcd[0] == 0x42);
}

// Only accesses that can change what the host sees poke it: writes, and FIFO accesses that hand a packet over.
static void usbNotifyTests() {
    S3cUsbDevice d;
    d.reset();
    assert(!d.hostVisible(0x184, false) && !d.hostVisible(0x198, false));
    assert(d.hostVisible(0x184, true) && d.hostVisible(0x178, true));
    // EP0's FIFO changes hands through EP0_CSR.
    assert(!d.hostVisible(0x1c0, true) && !d.hostVisible(0x1c0, false));
    // EP1: a byte read with OUT_PKT_RDY still set isn't seen, the one that clears it is.
    d.outCsr1[1] = 1;
    assert(!d.hostVisible(0x1c4, false));
    d.outCsr1[1] = 0;
    assert(d.hostVisible(0x1c4, false) && d.hostVisible(0x1c7, false));
    // A FIFO write is seen once IN_PKT_RDY is set (AUTO_SET).
    assert(!d.hostVisible(0x1c4, true));
    d.inCsr1[1] = 1;
    assert(d.hostVisible(0x1c4, true));
}

// The VBUS pin and the bridge's cable agree after a load: the saved GPG5 level goes, the cable state stays (the board
// sets GPG5 from usbVbus after a load, SocS3C2442::ApplyHostInputs).
static void vbusLoadTests() {
    auto a = make({loop});
    a->setUsbVbus(true);
    std::vector<uint8_t> state;
    std::string why;
    assert(a->saveState(state, why));
    auto b = make({loop});
    assert(b->loadState(state.data(), state.size(), why));
    assert(!b->usbVbus() && !(b->gpio.input[S3cGpio::g] & (1u << 5)));
    b->setUsbVbus(b->usbVbus());
    assert(!b->usbVbus() && (b->gpio.input[S3cGpio::g] & (1u << 5)));
}

// USB_INT_REG's SUSPEND_INT (bit 0) and the INT_USBD line, with suspend and reset interrupts on as WM5's driver
// has them while a host is attached.
static bool suspended(S3c2442Machine& m) {
    uint32_t v = 0;
    assert(m.usbDevice.read(0x158, v) == S3cAccess::ok);
    const bool on = v & 1;
    assert(m.usbDevice.irq() == on);
    if (on) assert(m.intc.srcpnd & (1u << s3cIrq::usbd));
    return on;
}

// A state saved with the cable in, opened again without one: the bus goes quiet and the guest sees suspend.
// Nothing is raised where there was no cable, and the cable going in raises nothing either.
static void cableRestoreTests() {
    auto a = make({loop});
    assert(a->usbDevice.write(0x16c, 0x05) == S3cAccess::ok);
    a->setUsbVbus(true);
    assert(!suspended(*a));
    std::vector<uint8_t> state;
    std::string why;
    assert(a->saveState(state, why));
    auto b = make({loop});
    assert(b->loadState(state.data(), state.size(), why));
    assert(!suspended(*b));
    assert(b->usbVbusOffAt() == UINT64_MAX);
    b->setUsbVbus(false);
    assert(suspended(*b) && b->usbVbusOffAt() == b->nanoseconds());
    // Reopened with the cable still in: no suspend.
    auto c = make({loop});
    assert(c->loadState(state.data(), state.size(), why));
    c->setUsbVbus(true);
    assert(!suspended(*c));
    // A machine that never had the cable, then the cable in and out live.
    auto d = make({loop});
    assert(d->usbDevice.write(0x16c, 0x05) == S3cAccess::ok);
    d->setUsbVbus(false);
    assert(!suspended(*d) && d->usbVbusOffAt() == UINT64_MAX);
    d->setUsbVbus(true);
    assert(!suspended(*d));
    d->setUsbVbus(false);
    assert(suspended(*d));
}

// The RTC opened again takes a set time in its registers (BCD, WM5's year from 1950, day of week + 1) and goes on
// counting from it; the alarm and tick settings are kept.
static void rtcRestoreTests() {
    auto a = make({loop});
    a->rtc.ticnt = 0x85;
    a->rtc.alarm[2] = 0x07;
    std::vector<uint8_t> state;
    std::string why;
    assert(a->saveState(state, why));
    auto b = make({loop});
    assert(b->loadState(state.data(), state.size(), why));
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;
    t.tm_mday = 25;
    t.tm_wday = 3;
    t.tm_hour = 7;
    t.tm_min = 41;
    t.tm_sec = 59;
    b->rtc.setTime(rx1950RtcTime(t));
    uint32_t v = 0;
    const uint32_t want[7] = {0x59, 0x41, 0x07, 0x25, 4, 0x11, 76};
    for (unsigned i = 0; i < 7; ++i) {
        assert(b->rtc.read(0x70 + 4 * i, v) == S3cAccess::ok);
        if (v != want[i]) std::fprintf(stderr, "rtc register %u: %x, wanted %x\n", i, v, want[i]);
        assert(v == want[i]);
    }
    assert(b->rtc.ticnt == 0x85 && b->rtc.alarm[2] == 0x07);
    b->rtc.advance(32768, b->intc);
    assert(b->rtc.read(0x70, v) == S3cAccess::ok && v == 0x00);
    assert(b->rtc.read(0x74, v) == S3cAccess::ok && v == 0x42);
}

// UART bit rates from the FCLK/n clock WM5 picks, and a byte from the far end raising the receive interrupt.
static void uartTests() {
    auto m = make(stores({{0x50000004, 0x4fc5}, {0x50000028, 15}, {0x50008000, 0x43}, {0x50008004, 0x8fc5},
                          {0x50008028, 191}}, {loop}));
    assert(m->run(1000) == S3c2442Stop::budget);
    const uint64_t fclk = m->clock.fclk();
    assert(m->uartBaud(0) == uint32_t(fclk / 10 / 16 / 16) && m->uartBaud(2) == uint32_t(fclk / 10 / 16 / 192));
    assert(m->uarts[2].infrared() && m->uarts[2].receiverOn());
    // UCON2 bit 15 off stops the FCLK/n clock: no bit rate on either.
    m->uarts[2].ucon &= ~0x8000u;
    assert(m->uartBaud(0) == 0 && m->uartBaud(2) == 0);
    m->uarts[2].ucon |= 0x8000;
    m->intc.intsubmsk = 0;
    m->uartReceive(2, 0xc0);
    assert((m->intc.srcpnd & (1u << s3cIrq::uart2)) && m->uarts[2].rx.size() == 1);
}

// Fast passes the WTCNT delay loops of eboot and wavedev.dll at once (skipDelay) with the same registers, flags,
// instructions and time as Exact at every stop. The same loops with cmp written as subs (same flags, so the same
// waits) aren't the code it knows, so nothing is skipped there.
static void delaySkipTests() {
    // mov r2, #0x53000000; mov r6, r2
    // ldr r4, [r2, #8]; sub r4, r4, #200
    // eboot's: ldr r3, [r2, #8]; cmp r3, r4; bhs back
    // add r7, r7, #1; ldr r5, [r6, #8]; mov r0, #0x40
    // wavedev's: ldr r3, [r6, #8]; sub r3, r5, r3; mov r3, r3, lsl #16; cmp r0, r3, lsr #16; bhs back
    // add r8, r8, #1; b to the first ldr
    const std::vector<uint32_t> base{0xe3a02453, 0xe1a06002, 0xe5924008, 0xe24440c8, 0xe5923008, 0xe1530004, 0x2afffffc,
                                  0xe2877001, 0xe5965008, 0xe3a00040, 0xe5963008, 0xe0453003, 0xe1a03803, 0xe1500823,
                                  0x2afffffa, 0xe2888001, 0xeafffff0};
    // Watchdog setups: WTCON, WTDAT and WTCNT start, whether its interrupt is unmasked (the CPU still has IRQs
    // masked), and the wavedev loop's variant: wrap has r0 all ones and r5 0x100 up, so the loop stays until the
    // 16-bit start - WTCNT wraps.
    struct Setup {
        uint32_t wtcon, wtcnt;
        bool irq, wrap;
    };
    const Setup setups[] = {
        {0x0220, 0xffff, false, false},  // no interrupt or reset
        {0x0224, 0x1000, true, false},   // interrupt: it limits each span
        {0x0221, 0x3000, false, false},  // reset: the run stops at the first run out
        {0x0225, 0x0900, true, false},   // both
        {0x0220, 0xffff, false, true},
        {0x0224, 0xffff, true, true},
    };
    for (const Setup& setup : setups)
    for (const bool known : {true, false}) {
        std::vector<uint32_t> program = base;
        if (setup.wrap) {
            program[9] = 0xe3e00000;  // mvn r0, #0
            program.insert(program.begin() + 10, 0xe2855c01);  // add r5, r5, #0x100
            program.back() = 0xeaffffef;  // b to the first ldr
        }
        if (!known) {
            program[5] = 0xe0539004;   // subs r9, r3, r4
            program[program.size() - 4] = 0xe0509823;  // subs r9, r0, r3, lsr #16
        }
        auto exact = make(program), fast = make(program);
        if (!fast->setFastEngine(true)) return;
        for (auto* m : {exact.get(), fast.get()}) {
            m->watchdog.wtdat = m->watchdog.wtcnt = setup.wtcnt;
            m->watchdog.wtcon = setup.wtcon;  // prescaler 2, divide by 16, counting
            if (setup.irq) {
                m->intc.intmsk &= ~(1u << s3cIrq::wdtAc97);
                m->intc.intsubmsk &= ~(1u << s3cSubIrq::wdt);
            }
        }
        for (const uint64_t slice : {1u, 7u, 1000u, 33333u, 250000u, 1000000u, 5000000u, 20000000u}) {
            exact->run(slice);
            fast->run(slice);
            for (unsigned r = 0; r < 17; ++r)
                if (cpuGetRegExternal(exact->cpu(), r) != cpuGetRegExternal(fast->cpu(), r)) {
                    std::fprintf(stderr, "delay skip: r%u %08x against %08x after %" PRIu64 " (pc %08x %08x, skips %" PRIu64 ")\n", r,
                                 cpuGetRegExternal(exact->cpu(), r), cpuGetRegExternal(fast->cpu(), r), exact->retired(),
                                 exact->pc(), fast->pc(), fast->delaySkips());
                    assert(false);
                }
            assert(exact->retired() == fast->retired() && exact->nanoseconds() == fast->nanoseconds());
            assert(exact->watchdog.wtcnt == fast->watchdog.wtcnt && exact->watchdog.phase == fast->watchdog.phase);
        }
        if (!known) assert(fast->delaySkips() == 0);
        else if (setup.wrap) assert(fast->delaySkips() > 2);
        else if (setup.wtcon == 0x0220) {
            // Both loops went round many times, through WTCNT reloads too.
            assert(cpuGetRegExternal(fast->cpu(), 7) > 50 && cpuGetRegExternal(fast->cpu(), 8) > 50);
            assert(fast->delaySkips() > 100 && fast->delaySkipRetired() > fast->retired() / 2);
        } else assert(fast->delaySkips() > 0);
    }
}

// The other polls skipDelay takes, Exact against Fast at every stop: the microsecond delay with lr, r1 and r2
// (017150bc), and the touch driver's ADCCON wait for ECFLG (01711890) over conversions started one after another.
static void pollSkipTests() {
    auto same = [](S3c2442Machine& exact, S3c2442Machine& fast) {
        for (const uint64_t slice : {1u, 7u, 1000u, 33333u, 250000u, 1000000u, 5000000u}) {
            exact.run(slice);
            fast.run(slice);
            for (unsigned r = 0; r < 17; ++r)
                if (cpuGetRegExternal(exact.cpu(), r) != cpuGetRegExternal(fast.cpu(), r)) {
                    std::fprintf(stderr, "poll skip: r%u %08x against %08x after %" PRIu64 " (pc %08x %08x, skips %" PRIu64 ")\n", r,
                                 cpuGetRegExternal(exact.cpu(), r), cpuGetRegExternal(fast.cpu(), r), exact.retired(),
                                 exact.pc(), fast.pc(), fast.delaySkips());
                    assert(false);
                }
            assert(exact.retired() == fast.retired() && exact.nanoseconds() == fast.nanoseconds());
        }
    };
    // mov lr, #0x53000000; ldr r1, [lr, #8]; mov r2, #0x40
    // ldr r3, [lr, #8]; sub r3, r1, r3; mov r3, r3, lsl #16; cmp r2, r3, lsr #16; bhs back
    // add r8, r8, #1; b to the first ldr
    for (const bool known : {true, false}) {
        std::vector<uint32_t> program{0xe3a0e453, 0xe59e1008, 0xe3a02040, 0xe59e3008, 0xe0413003,
                                      0xe1a03803, 0xe1520823, 0x2afffffa, 0xe2888001, 0xeafffff6};
        if (!known) program[4] = 0xe0513003;  // subs r3, r1, r3
        auto exact = make(program), fast = make(program);
        if (!fast->setFastEngine(true)) return;
        for (auto* m : {exact.get(), fast.get()}) {
            m->watchdog.wtdat = m->watchdog.wtcnt = 0xffff;
            m->watchdog.wtcon = 0x0220;
        }
        same(*exact, *fast);
        assert(cpuGetRegExternal(fast->cpu(), 8) > 50);
        assert(known ? fast->delaySkips() > 50 : fast->delaySkips() == 0);
    }
    // mov r4, #0x58000000; ldr r0, [r4]; orr r0, r0, #1; str r0, [r4] (ENABLE_START)
    // ldr r3, [r4]; tst r3, #0x8000; beq back
    // add r7, r7, #1; ldr r5, [r4, #12]; b to the first ldr
    for (const unsigned variant : {0u, 1u, 2u}) {
        std::vector<uint32_t> program{0xe3a04458, 0xe5940000, 0xe3800001, 0xe5840000, 0xe5943000,
                                      0xe3130902, 0x0afffffc, 0xe2877001, 0xe594500c, 0xeafffff6};
        if (variant == 2) program[5] = 0xe2139902;  // ands r9, r3, #0x8000
        auto exact = make(program), fast = make(program);
        if (!fast->setFastEngine(true)) return;
        for (auto* m : {exact.get(), fast.get()}) {
            m->adc.adccon = (1u << 14) | (49u << 6);  // prescaler on, 50, out of standby
            if (variant == 1) {
                m->adc.adctsc = 4;  // AUTO_PST: ADCDLY first
                m->adc.adcdly = 0x2000;
            }
        }
        same(*exact, *fast);
        assert(cpuGetRegExternal(fast->cpu(), 7) > 10 && fast->adc.conversions > 10);
        assert(variant == 2 ? fast->delaySkips() == 0 : fast->delaySkips() > 10);
    }
}

// TCNTO4 reads at access pcs that start their block (as the WM5 tick read), Exact against Fast at every stop,
// on the compile thread: a marked ldrne whose condition fails, a marked load of RAM, a marked load after a run
// of unmarked instructions, which splits the block there (a marked pc can't sit mid-block with startBlocks), and
// a marked load to pc, which ends its block. Each leaves nothing behind for the unmarked TCNTO4 read after it,
// which Fast blocks mustn't answer, and the third is answered with the exact time.
static void tickReadTests() {
    // mov r0, #0x51000000; mov r6, #0x30000000; add r6, r6, #0x10000
    // back: cmp r3, r3; ldrne r1, [r0, #0x40] (marked); add r4, r4, #1 x64; ldr r5, [r0, #0x40]; add r7, r7, r5
    // ldr r1, [r6] (marked); add r4, r4, #1 x64; ldr r8, [r0, #0x40]; add r9, r9, r8; add r4, r4, #1 x32
    // ldr r10, [r0, #0x40] (marked); add r11, r11, r10; ldr pc, [pc, #-4] (marked) to the next but one word;
    // ldreq r2, [r0, #0x40]; add r4, r4, #1 x32; ldr r2, [r0, #0x40]; add r9, r9, r2; mov r12, #0x800; subs r12, r12, #1; bne back 1
    // add r3, r3, #1; b back
    std::vector<uint32_t> program{0xe3a00451, 0xe3a06203, 0xe2866801, 0xe1530003};
    static uint32_t marked[4];
    marked[0] = code + uint32_t(program.size()) * 4;
    program.push_back(0x15901040);
    program.insert(program.end(), 64, 0xe2844001);
    program.insert(program.end(), {0xe5905040, 0xe0877005});
    marked[1] = code + uint32_t(program.size()) * 4;
    program.push_back(0xe5961000);
    program.insert(program.end(), 64, 0xe2844001);
    program.insert(program.end(), {0xe5908040, 0xe0899008});
    program.insert(program.end(), 32, 0xe2844001);
    marked[2] = code + uint32_t(program.size()) * 4;
    program.insert(program.end(), {0xe590a040, 0xe08bb00a});
    marked[3] = code + uint32_t(program.size()) * 4;
    program.insert(program.end(), {0xe51ff004, marked[3] + 8, 0x05902040});
    program.insert(program.end(), 32, 0xe2844001);
    program.insert(program.end(), {0xe5902040, 0xe0899002, 0xe3a0cb02, 0xe25cc001, 0x1afffffd, 0xe2833001});
    program.push_back(0xea000000u | ((3 - int32_t(program.size()) - 2) & 0xffffff));
    auto exact = make(program), fast = make(program);
    if (!fast->setFastEngine(true)) return;
    cpuArm920SetJitAccessPcs(fast->cpu(), marked, 4, true);
    fast->mmioTrace = [](uint32_t pc, uint32_t address, unsigned, bool write, uint32_t) {
        if (address == 0x51000040 && !write) assert(pc != marked[3]);
    };
    for (auto* m : {exact.get(), fast.get()}) {
        m->pwm.tcfg0 = 0;  // prescaler 0 and the 1/2 divider: TCNTO4 moves every 2 PCLK ticks
        m->pwm.tcfg1 = 0;
        pwmTimer4(*m, 0xffff);
    }
    for (const uint64_t slice : {1u, 7u, 1000u, 33333u, 250000u, 1000000u, 5000000u}) {
        exact->run(slice);
        fast->run(slice);
        for (unsigned r = 0; r < 17; ++r)
            if (cpuGetRegExternal(exact->cpu(), r) != cpuGetRegExternal(fast->cpu(), r)) {
                std::fprintf(stderr, "tick read: r%u %08x against %08x after %" PRIu64 " (pc %08x %08x)\n", r,
                             cpuGetRegExternal(exact->cpu(), r), cpuGetRegExternal(fast->cpu(), r), exact->retired(),
                             exact->pc(), fast->pc());
                assert(false);
            }
        assert(exact->retired() == fast->retired() && exact->nanoseconds() == fast->nanoseconds());
    }
    assert(cpuGetRegExternal(fast->cpu(), 3) > 1000);
    assert(fast->jitRetired() > fast->retired() / 2);
}

int main() {
    tickReadTests();
    pollSkipTests();
    delaySkipTests();
    usbTests();
    hostServiceTests();
    hostServiceSleepTests();
    hostScheduleStateTests();
    watchdogHookTests();
    powerOnResetTests();
    usbNotifyTests();
    vbusLoadTests();
    cableRestoreTests();
    rtcRestoreTests();
    uartTests();
    sdTests();
    audioTests();
    wifiTests();
    // A masked RTC tick can't end a wait for interrupt.
    {
        auto m = make({wfi, loop});
        m->rtc.ticnt = 0x80;
        assert(m->run(1000000000ull) == S3c2442Stop::idleNoWake);
    }
    // Unmasked, it does, and the CPU takes the IRQ. The tick is every 1/128 s, 8.4 M cycles at the reset
    // PLL, and idle cycles count against the budget.
    {
        auto m = make({0xe321f013, wfi, loop});  // msr cpsr_c, #0x13
        m->rtc.ticnt = 0x80;
        m->intc.intmsk &= ~(1u << s3cIrq::tick);
        const auto stop = m->run(20000000);
        if (stop != S3c2442Stop::budget || mode(*m) != 0x12)
            std::fprintf(stderr, "tick wake: %s, mode %x, pc %08x\n", m->stopText().c_str(), mode(*m), m->pc());
        assert(stop == S3c2442Stop::budget && mode(*m) == 0x12);
    }
    // A masked auto-reload PWM timer under CLKCON IDLE.
    {
        auto m = make({loop});
        pwmTimer4(*m, 100);
        m->clock.clkcon |= 4;
        assert(m->run(1000000000ull) == S3c2442Stop::idleNoWake);
    }
    // The watchdog with WTCNT and WTDAT 0 never runs out, with the interrupt or the reset on.
    for (uint32_t on : {4u, 1u}) {
        auto m = make({wfi, loop});
        m->watchdog.wtdat = m->watchdog.wtcnt = 0;
        m->watchdog.wtcon = 0x20 | on;
        m->intc.intmsk &= ~(1u << s3cIrq::wdtAc97);
        m->intc.intsubmsk &= ~(1u << s3cSubIrq::wdt);
        assert(m->run(1000000000ull) == S3c2442Stop::idleNoWake);
    }
    // Idle time counts against the budget: an unmasked timer far off doesn't run past it.
    {
        auto m = make({wfi, loop});
        m->pwm.write(0x00, 0xff00);  // prescaler 255 for timers 2-4
        m->pwm.write(0x04, 3u << 16);  // divider 1/16 for timer 4
        pwmTimer4(*m, 0xffff);
        m->intc.intmsk &= ~(1u << s3cIrq::timer4);
        assert(m->run(1000) == S3c2442Stop::budget);
        assert(m->retired() + m->idleCycles() <= 1000 + 64 && m->idleCycles() > 0);
    }
    // An undefined instruction stops the run; after start() the next run doesn't stop for it again.
    {
        auto m = make({undefined, loop});
        assert(m->run(1000) == S3c2442Stop::undefined && m->stopInfo().pc == code);
        const uint32_t again[] = {loop};
        m->loadRam(code, reinterpret_cast<const uint8_t*>(again), 4);
        m->start(code);
        assert(m->run(1000) == S3c2442Stop::budget);
    }
    // A real undefined instruction followed by a VFP probe in the same batch still stops, at the first one.
    // The undefined vector returns to the next instruction.
    {
        constexpr uint32_t vfp = 0xeef00a10, returnNext = 0xe1b0f00e;  // fmrx r0, fpsid; movs pc, lr
        auto m = make({undefined, vfp, loop});
        std::memcpy(m->steppingstone + 4, &returnNext, 4);
        assert(m->run(1000) == S3c2442Stop::undefined && m->stopInfo().pc == code);
        assert(m->vfpProbeCount() == 1);
        auto n = make({vfp, undefined, loop});
        std::memcpy(n->steppingstone + 4, &returnNext, 4);
        assert(n->run(1000) == S3c2442Stop::undefined && n->stopInfo().pc == code + 4);
    }
    // Host input: the power key on GPF0 as EINT0 on a falling edge, and the stylus down with the touch
    // screen waiting for it, each raise their interrupt at once.
    {
        auto m = make({loop});
        m->gpio.write(0x88, 2, m->intc);  // EXTINT0: EINT0 on a falling edge
        m->gpio.write(0x50, 2, m->intc);  // GPFCON: GPF0 as EINT0
        m->setPin(S3cGpio::f, 0, false);
        assert(m->intc.pending() & (1u << s3cIrq::eint0));
        m->setPin(S3cGpio::f, 0, true);
        m->adc.write(0x04, 0xd3, m->intc);
        m->intc.intsubmsk &= ~(1u << s3cSubIrq::tc);
        m->setStylus(true, 100, 200);
        assert((m->intc.pending() & (1u << s3cIrq::adc)) && m->adc.stylusDown());
    }
    // An SFR write that unmasks a pending interrupt: the CPU runs the next instruction before it takes the
    // IRQ. UART0's level TX interrupt is pending with INT_UART0 open; the guest unmasks TXD0, then sets r2.
    {
        auto m = make({0xe321f013, 0xe3a0044a, 0xe3e01002, 0xe580101c, 0xe3a02001, loop});
        // msr cpsr_c, #0x13; mov r0, #0x4a000000; mvn r1, #2; str r1, [r0, #0x1c]; mov r2, #1
        m->uarts[0].write(0x04, (1u << 2) | 0x200, m->intc);
        m->intc.intmsk &= ~(1u << s3cIrq::uart0);
        assert(!(m->intc.pending() & (1u << s3cIrq::uart0)));
        assert(m->run(1000) == S3c2442Stop::budget);
        assert(mode(*m) == 0x12 && cpuGetRegExternal(m->cpu(), 2) == 1);
    }
    // The rx1950 with no serial cable reads GPF6 low; every other pin stays high.
    {
        S3c2442Options options;
        options.pinLevels[rx1950CablePort] &= ~(1u << rx1950CableBit);
        auto m = make({loop}, options);
        uint32_t gpf = 0;
        assert(m->gpio.read(0x54, gpf) == S3cAccess::ok && (gpf & 0xff) == 0xbf);
    }
    // Fast against Exact on a WTCNT polling loop (the lean path's SFR read) with the watchdog counting and
    // its interrupt raised (IRQs stay masked): the same registers, instructions and time at every stop.
    // mov r0, #0x53000000; ldr r1, [r0, #8]; add r2, r2, r1; add r3, r3, #1; b to the ldr
    const std::vector<uint32_t> poll{0xe3a00453, 0xe5901008, 0xe0822001, 0xe2833001, 0xeafffffb};
    {
        auto exact = make(poll), fast = make(poll);
        if (fast->setFastEngine(true)) {
            for (auto* m : {exact.get(), fast.get()}) {
                m->watchdog.wtdat = m->watchdog.wtcnt = 0x400;
                m->watchdog.wtcon = 0x0124;  // prescaler 1, divide by 16, counting, interrupt, no reset
                m->intc.intmsk &= ~(1u << s3cIrq::wdtAc97);
                m->intc.intsubmsk &= ~(1u << s3cSubIrq::wdt);
            }
            for (const uint64_t slice : {1u, 7u, 1000u, 33333u, 250000u, 1000000u}) {
                exact->run(slice);
                fast->run(slice);
                for (unsigned r = 0; r < 16; ++r)
                    assert(cpuGetRegExternal(exact->cpu(), r) == cpuGetRegExternal(fast->cpu(), r));
                assert(exact->retired() == fast->retired() && exact->nanoseconds() == fast->nanoseconds());
            }
            assert(fast->leanRetired() + fast->jitRetired() > fast->retired() / 2);
        }
    }
    // The battery setting on the rx1950's inputs: every level battdrvr can read back reads back exactly through
    // its own sums; the ones no count gives read the next percent up. All stay above its 3300 mV power off.
    {
        bool reachable[101] = {};
        for (unsigned c = 0; c < 1024; ++c) {
            const unsigned got = rx1950GuestPercent(rx1950CountMilliVolts(uint16_t(c)));
            if (got <= 100) reachable[got] = true;
        }
        const unsigned unreachable[] = {18, 26, 29, 32, 35, 38, 41, 44, 47};
        for (unsigned p = 0; p <= 100; ++p) {
            const bool listed = std::find(std::begin(unreachable), std::end(unreachable), p) != std::end(unreachable);
            assert(reachable[p] == !listed);
            const unsigned mv = rx1950CountMilliVolts(rx1950BatteryAin0(p));
            const unsigned got = rx1950GuestPercent(mv);
            if (got != p + listed) std::fprintf(stderr, "battery %u%%: %u mV reads %u%%\n", p, mv, got);
            assert(got == p + listed && mv > 3300);
        }
    }
    {
        BatteryPower full;
        assert(rx1950CountMilliVolts(rx1950PowerInputs(full).adc[0]) >= 4120);
    }
    // Each level and charging state on a running machine: AIN0-3, GPF2 (low with AC) and GPF3 (high when the
    // charge is done or off the charger), and EINT2 on both edges as the OAL sets it, raised by plugging in
    // and pulling out.
    {
        BatteryPower start;
        assert(BatteryPower::Parse("level 30 battery", start));
        S3c2442Options options;
        rx1950BatteryPins(start, options);
        auto m = make({loop}, options);
        rx1950ApplyBattery(*m, start);
        assert(m->gpio.write(0x50, 2u << 4, m->intc) == S3cAccess::ok);           // GPF2 as EINT2
        assert(m->gpio.write(0x88, 6u << 8, m->intc) == S3cAccess::ok);           // EINT2 both edges
        m->intc.srcpnd = 0;
        struct Case { const char* spec; bool ac, done; bool edge; };
        const Case cases[] = {
            {"level 30 battery", false, true, false},  {"level 30 charging", true, false, true},
            {"level 5 charging", true, false, false}, {"max charging", true, true, false},
            {"level 100 battery", false, true, true},  {"host 0 battery", false, true, false},
            {"host 70 charging", true, false, true},   {"max battery", false, true, true},
        };
        for (const auto& c : cases) {
            BatteryPower b;
            assert(BatteryPower::Parse(c.spec, b));
            rx1950ApplyBattery(*m, b);
            uint32_t gpf = 0, adcdat = 0;
            assert(m->gpio.read(0x54, gpf) == S3cAccess::ok);
            assert(bool(gpf & 4) == !c.ac && bool(gpf & 8) == c.done);
            assert(bool(m->intc.srcpnd & (1u << s3cIrq::eint2)) == c.edge);
            m->intc.srcpnd = 0;
            assert(rx1950GuestPercent(rx1950CountMilliVolts(m->adc.channel[0])) + 1 >= b.Percent());
            assert(m->adc.channel[1] == 0 && m->adc.channel[2] == 451 && m->adc.channel[3] == rx1950PackIdCount);
            // AIN0 through a normal conversion, as battdrvr reads it: ADCCON prescaler 49, channel 0, start.
            m->adc.write(0, 0x4c40 | 1, m->intc);
            m->run(100000);
            assert(m->adc.read(0x0c, adcdat) == S3cAccess::ok && (adcdat & 0x3ff) == m->adc.channel[0]);
            assert(rx1950BatteryState(b) == b.Describe() && rx1950BatteryState(b).find("no-charging") == std::string::npos);
        }
        // The USB cable is AC: GPF2 low and the charger on below 100% with the setting on battery, EINT2 on
        // plugging in and pulling out, and back to the setting's own state after.
        {
            BatteryPower b;
            assert(BatteryPower::Parse("level 60 battery", b));
            rx1950ApplyBattery(*m, b);
            m->intc.srcpnd = 0;
            uint32_t gpf = 0;
            m->setUsbVbus(true);
            rx1950ApplyBattery(*m, b);
            assert(m->gpio.read(0x54, gpf) == S3cAccess::ok && !(gpf & 4) && !(gpf & 8));
            assert(m->intc.srcpnd & (1u << s3cIrq::eint2));
            m->intc.srcpnd = 0;
            BatteryPower full;
            rx1950ApplyBattery(*m, full);
            assert(m->gpio.read(0x54, gpf) == S3cAccess::ok && !(gpf & 4) && (gpf & 8));
            m->setUsbVbus(false);
            rx1950ApplyBattery(*m, b);
            assert(m->gpio.read(0x54, gpf) == S3cAccess::ok && (gpf & 4) && (gpf & 8));
            assert(m->intc.srcpnd & (1u << s3cIrq::eint2));
            m->intc.srcpnd = 0;
        }
        // Pack ID 0 (500-700 mV, no 1.3191 divider) and the thermistor at 25 C.
        const unsigned packMv = rx1950PackIdCount * 3300 / 1023;
        assert(packMv >= 500 && packMv <= 700);
    }
    // Sleep: CLKCON POWER_OFF stops the CPU, time goes on for the RTC only, and the power key (EINT0 on a
    // falling edge) wakes it with the wake-up reset: GSTATUS2 OFFRST, the source pending, blocks reset, the
    // resetLoader's start, the GPIO block kept. Then the RTC alarm wakes it on its own.
    // mov r0, #0x4c000000; mov r1, #8; str r1, [r0, #0xc] (CLKCON POWER_OFF); b .
    for (const bool fast : {false, true}) {
        std::vector<uint32_t> program{0xe3a0044c, 0xe3a01008, 0xe580100c, loop};
        program.resize(0x100 / 4 + 1, 0);
        program.back() = loop;  // where the loader starts
        auto m = make(program);
        if (fast && !m->setFastEngine(true)) continue;
        unsigned loads = 0;
        // The loader stand-in turns the watchdog off, so its reset value doesn't reset the loop.
        m->resetLoader = [&] { ++loads; m->watchdog.wtcon = 0; m->start(code + 0x100); };
        assert(m->gpio.write(0x50, 2, m->intc) == S3cAccess::ok);  // GPF0 as EINT0
        assert(m->gpio.write(0x88, 2, m->intc) == S3cAccess::ok);  // falling edge
        m->pwm.timers[0].buffer = 150;
        m->pwm.timers[0].compare = 75;
        m->pwm.tcon = 1;
        assert(m->gpio.write(0x10, 2, m->intc) == S3cAccess::ok);  // GPB0 as TOUT0
        assert(m->backlightLevel() == 76 * 1000 / 151);
        // TCMPB0 above TCNTB0: the count never meets the compare, TOUT0 stays at its idle level (low, or high
        // with the inverter on).
        m->pwm.timers[0].compare = 200;
        assert(m->backlightLevel() == 0);
        m->pwm.tcon = 1 | 4;
        assert(m->backlightLevel() == 1000);
        m->pwm.timers[0].compare = 150;
        assert(m->backlightLevel() == 0);
        m->pwm.tcon = 1;
        assert(m->backlightLevel() == 1000);
        m->pwm.timers[0].compare = 75;
        assert(m->run(1000) == S3c2442Stop::budget && m->asleep() && m->sleepCount() == 1);
        assert(m->backlightLevel() == -1);
        const uint64_t ns = m->nanoseconds(), retired = m->retired();
        assert(m->run(50000000) == S3c2442Stop::budget && m->asleep() && m->retired() == retired && m->nanoseconds() > ns);
        assert(m->pwm.tcon == 1);  // nothing but the RTC moves asleep
        m->setPin(S3cGpio::f, 0, false);
        m->run(10);
        assert(!m->asleep() && m->wakeCount() == 1 && loads == 1 && m->gpio.gstatus[2] == 2 && m->lastWakeSources() == 1);
        assert((m->intc.srcpnd & 1) && m->intc.intmsk == 0xffffffff && m->pwm.tcon == 0 && !m->clock.powerOff());
        assert(m->gpio.con[S3cGpio::f] == 2 && m->pc() == code + 0x100);
        m->setPin(S3cGpio::f, 0, true);
        // Asleep again (from the start of the program), then the alarm two seconds on.
        m->start(code);
        m->rtc.rtcalm = 0x41;  // ALMEN, seconds
        const unsigned second = ((m->rtc.bcd[0] >> 4) * 10 + (m->rtc.bcd[0] & 0xf) + 2) % 60;
        m->rtc.alarm[0] = (second / 10) << 4 | second % 10;
        m->intc.srcpnd = 0;
        assert(m->run(1000) == S3c2442Stop::budget && m->asleep());
        m->run(4000000000ull);
        assert(!m->asleep() && m->wakeCount() == 2 && (m->intc.srcpnd & (1u << s3cIrq::rtc)));
        assert(m->lastWakeSources() == 1u << s3cIrq::rtc);
    }
    // The LEDs from the OAL's LED states (nk 0x800529c8) once the loader has made their pins outputs.
    {
        auto m = make({loop});
        rx1950LoaderPins(m->gpio);
        assert((m->gpio.con[S3cGpio::b] & 3) == 2);
        auto gpa = [&](uint32_t v, bool gpj6) {
            m->gpio.dat[S3cGpio::a] = v;
            m->gpio.dat[S3cGpio::j] = gpj6 ? 0x40 : 0;
        };
        gpa(0x201000, false);  // all off
        auto l = rx1950Leds(*m);
        assert(!l.any() && !rx1950NotificationLed(*m));
        gpa(0x201000 | 0x18, true);  // charging: amber blinking
        l = rx1950Leds(*m);
        assert(l.green && l.red && l.blinking && !l.blue && !rx1950NotificationLed(*m));
        gpa(0x201000 | 0xc0, false);  // charged
        l = rx1950Leds(*m);
        assert(l.green && l.red && !l.blinking);
        gpa(0x201000 | 0x10, true);  // notification
        l = rx1950Leds(*m);
        assert(l.green && !l.red && l.blinking && rx1950NotificationLed(*m));
        gpa(0x201000 | 0x800, false);  // wireless
        assert(rx1950Leds(*m).blue);
        // SoC::Attention's colour: blinked pins only in the lit half of each second, solid ones always.
        gpa(0x201000 | 0x18, true);
        assert(rx1950LedColour(*m, 0) == 3 && rx1950LedColour(*m, 499999999) == 3 && rx1950LedColour(*m, 500000000) == 0);
        assert(rx1950LedColour(*m, 1000000000) == 3);
        gpa(0x201000 | 0xc0, false);
        assert(rx1950LedColour(*m, 0) == 3 && rx1950LedColour(*m, 700000000) == 3);
        gpa(0x201000 | 0x10, true);
        assert(rx1950LedColour(*m, 100) == 1 && rx1950LedColour(*m, 600000000) == 0);
        gpa(0x201000 | 0x800, false);
        assert(rx1950LedColour(*m, 0) == 0);
    }
    // Save and load: a machine loaded from a state runs on exactly as the one saved, with a timer interrupt; a damaged state or one from another SDRAM size is refused and changes nothing.
    {
        const std::vector<uint32_t> counter = {0xe3a01203, 0xe2811801, 0xe2800001, 0xe5810000, 0xeafffffc};
        auto a = make(counter);
        pwmTimer4(*a, 500);
        a->intc.intmsk &= ~(1u << s3cIrq::timer4);
        a->rtc.ticnt = 0x81;
        assert(a->run(300000) == S3c2442Stop::budget);
        std::vector<uint8_t> saved;
        std::string why;
        assert(a->saveState(saved, why));
        assert(a->run(500000) == S3c2442Stop::budget);

        auto b = make({loop});
        assert(b->loadState(saved.data(), saved.size(), why));
        assert(b->run(500000) == S3c2442Stop::budget);
        for (unsigned r = 0; r < 16; ++r)
            assert(cpuGetRegExternal(a->cpu(), r) == cpuGetRegExternal(b->cpu(), r));
        assert(cpuGetRegExternal(a->cpu(), ARM_REG_NUM_CPSR) == cpuGetRegExternal(b->cpu(), ARM_REG_NUM_CPSR));
        assert(cpuGetRegExternal(a->cpu(), 0) > 1000 && (a->intc.srcpnd & (1u << s3cIrq::timer4)));
        assert(a->retired() == b->retired() && a->nanoseconds() == b->nanoseconds());
        assert(a->pwm.ticksToEvent() == b->pwm.ticksToEvent() && a->rtc.ticnt == b->rtc.ticnt);
        assert(a->intc.srcpnd == b->intc.srcpnd && a->sleepCount() == b->sleepCount());

        auto c = make({loop});
        const uint32_t before = c->pc();
        std::vector<uint8_t> broken = saved;
        broken[broken.size() / 2] ^= 1;
        assert(!c->loadState(broken.data(), broken.size(), why) && !why.empty());
        assert(!c->loadState(saved.data(), saved.size() / 2, why));
        S3c2442Options other;
        other.ramBytes = 64u << 20;
        auto d = S3c2442Machine::create(other, why);
        assert(d && !d->loadState(saved.data(), saved.size(), why) && why.find("another machine") != std::string::npos);
        assert(c->pc() == before && c->run(1000) == S3c2442Stop::budget && c->pc() == code);

        // Fast picks up from a loaded state too.
        auto f = make({loop});
        assert(f->setFastEngine(true));
        assert(f->loadState(saved.data(), saved.size(), why));
        assert(f->run(500000) == S3c2442Stop::budget);
        assert(cpuGetRegExternal(f->cpu(), 0) > cpuGetRegExternal(a->cpu(), 0) / 2);
    }
    // Save while asleep, as quitting does, and load it back: the copy is asleep with the same counts, and the
    // power key wakes both the same way.
    {
        std::vector<uint32_t> program{0xe3a0044c, 0xe3a01008, 0xe580100c, loop};
        program.resize(0x100 / 4 + 1, 0);
        program.back() = loop;
        auto a = make(program), b = make({loop});
        for (auto* m : {a.get(), b.get()}) m->resetLoader = [m] { m->watchdog.wtcon = 0; m->start(code + 0x100); };
        assert(a->gpio.write(0x50, 2, a->intc) == S3cAccess::ok && a->gpio.write(0x88, 2, a->intc) == S3cAccess::ok);
        assert(a->run(1000) == S3c2442Stop::budget && a->asleep());
        assert(a->run(30000000) == S3c2442Stop::budget);
        std::vector<uint8_t> saved;
        std::string why;
        assert(a->saveState(saved, why));
        assert(b->loadState(saved.data(), saved.size(), why));
        assert(b->asleep() && b->sleepCount() == 1 && b->nanoseconds() == a->nanoseconds());
        for (auto* m : {a.get(), b.get()}) {
            assert(m->run(30000000) == S3c2442Stop::budget && m->asleep());
            m->setPin(S3cGpio::f, 0, false);
            m->run(1000);
        }
        assert(!a->asleep() && !b->asleep() && a->wakeCount() == 1 && b->wakeCount() == 1);
        assert(a->pc() == b->pc() && a->pc() == code + 0x100 && b->gpio.gstatus[2] == 2);
        assert(a->nanoseconds() == b->nanoseconds() && a->rtc.bcd[0] == b->rtc.bcd[0]);
    }
    // Save with the MMU, FCSE PID and D-cache on and a dirty line, and with a NAND page program half written:
    // the copy runs on to the same registers, SDRAM and NAND page. Exact, which keeps cache contents.
    {
        // TTB r4, DACR r5, PID r3, control r6; a read of r1 to bring its line in (the cache allocates on
        // reads only); then r0 += 1, stored at r1 (VA 0x30010000, write-back, so the line stays dirty) and r2
        // (VA 0x10000, PID 3: MVA 0x06010000, phys 0x30210000).
        const std::vector<uint32_t> program{mcr(2, 4), mcr(3, 5), mcr(13, 3), mcr(1, 6), 0xe5917000,
                                            0xe2800001, 0xe5810000, 0xe5820000, 0xeafffffb};
        auto setUp = [&](S3c2442Machine& m) {
            const uint32_t ttb = 0x30004000;
            auto section = [&](uint32_t va, uint32_t pa, bool cached) {
                const uint32_t entry = pa | 0xc00 | (cached ? 0xc : 0) | 2;
                assert(m.loadRam(ttb + (va >> 20) * 4, reinterpret_cast<const uint8_t*>(&entry), 4));
            };
            section(0x00000000, 0x00000000, false);
            section(0x30000000, 0x30000000, true);
            section(0x06000000, 0x30200000, true);
            cpuSetReg(m.cpu(), 1, 0x30010000);
            cpuSetReg(m.cpu(), 2, 0x00010000);
            cpuSetReg(m.cpu(), 3, 3u << 25);
            cpuSetReg(m.cpu(), 4, ttb);
            cpuSetReg(m.cpu(), 5, 0xffffffff);
            cpuSetReg(m.cpu(), 6, 0x78 | 0x1 | 0x4 | 0x1000);
        };
        auto a = make(program);
        setUp(*a);
        assert(a->run(100003) == S3c2442Stop::budget);
        const uint32_t count = cpuGetRegExternal(a->cpu(), 0);
        uint32_t inRam = 0;
        std::memcpy(&inRam, a->sdram() + 0x10000, 4);
        assert(count > 1000 && inRam != count);  // the line is dirty in the D-cache
        // NAND: page program of page 5, 16 bytes in.
        const uint32_t page = 5;
        assert(a->nand.write(0x04, 4, 0x381, a->intc) == S3cAccess::ok);  // controller on, chip selected
        assert(a->nand.write(0x08, 1, 0x80, a->intc) == S3cAccess::ok);
        for (const uint32_t byte : {0u, page & 0xff, (page >> 8) & 0xff, page >> 16})
            assert(a->nand.write(0x0c, 1, byte, a->intc) == S3cAccess::ok);
        for (uint32_t i = 0; i < 4; ++i) assert(a->nand.write(0x10, 4, 0x11223344u + i, a->intc) == S3cAccess::ok);
        std::vector<uint8_t> saved;
        std::string why;
        assert(a->saveState(saved, why));
        auto b = make({loop});
        assert(b->loadState(saved.data(), saved.size(), why));
        assert(cpuGetPid(b->cpu()) == 3u << 25);
        for (auto* m : {a.get(), b.get()}) {
            for (uint32_t i = 0; i < 4; ++i) assert(m->nand.write(0x10, 4, 0x55667788u + i, m->intc) == S3cAccess::ok);
            assert(m->nand.write(0x08, 1, 0x10, m->intc) == S3cAccess::ok);
            assert(m->run(200000) == S3c2442Stop::budget);
        }
        for (unsigned r = 0; r < 16; ++r) assert(cpuGetRegExternal(a->cpu(), r) == cpuGetRegExternal(b->cpu(), r));
        assert(a->retired() == b->retired() && std::memcmp(a->sdram(), b->sdram(), 32u << 20) == 0);
        // No store and no image: the program made the flash, erased, and wrote the page into it.
        assert(a->nand.flash.size() == size_t(S3cNand::pages) * S3cNand::pageSize && a->nand.flash == b->nand.flash);
        const uint8_t* p = &b->nand.flash[size_t(page) * S3cNand::pageSize];
        assert(p[0] == 0x44 && p[16] == 0x88 && p[32] == 0xff && a->nand.flash[0] == 0xff && b->nand.programs == 1);
    }
    // The watchdog's reset restarts the machine through resetLoader with GSTATUS2 WDTRST, keeping SDRAM, the
    // RTC's time and the board's pins. Both engines.
    for (const bool fast : {false, true}) {
        std::vector<uint32_t> program{0xe2800001, loop - 1};  // add r0, r0, #1; b to the add
        program.resize(0x100 / 4 + 1, 0);
        program.back() = loop;
        auto m = make(program);
        if (fast && !m->setFastEngine(true)) continue;
        unsigned loads = 0;
        m->resetLoader = [&] { ++loads; m->start(code + 0x100); };
        const uint32_t marker = 0x5eed1950;
        assert(m->loadRam(code + 0x200000, reinterpret_cast<const uint8_t*>(&marker), 4));
        m->setPin(S3cGpio::g, 1, false);
        m->rtc.bcd[6] = 0x26;  // a year the RTC's reset wouldn't give
        m->gpio.gstatus[2] = 0;
        // Prescaler 0, divide by 16, counting, reset on: 0x100 counts are 4096 PCLK ticks.
        m->watchdog.wtcnt = m->watchdog.wtdat = 0x100;
        m->watchdog.wtcon = 0x0021;
        assert(m->run(100000) == S3c2442Stop::budget);
        assert(m->watchdogResets() == 1 && loads == 1 && m->gpio.gstatus[2] == 4 && m->pc() == code + 0x100);
        assert(m->watchdog.wtcon == 0x8021 && !m->watchdog.resetRequested && m->intc.intmsk == 0xffffffff);
        uint32_t kept = 0;
        std::memcpy(&kept, m->sdram() + 0x200000, 4);
        assert(kept == marker && m->rtc.bcd[6] == 0x26 && !(m->gpio.input[S3cGpio::g] & 2));
        // nRESET gives no reset bit.
        m->reset();
        assert(m->gpio.gstatus[2] == 0 && loads == 2 && m->watchdogResets() == 1);
    }
    // Pen pacing: a down and an up queued together reach the touch panel at least holdNs of guest time
    // apart, and a second tap waits gapNs after the up.
    {
        auto m = make({loop});
        S3cPenPacer pen;
        pen.clear(*m);
        pen.push(true, 300, 400);
        pen.push(false, 0, 0);
        pen.push(true, 310, 410);
        pen.push(false, 0, 0);
        std::vector<std::pair<bool, uint64_t>> seen;
        bool was = false;
        while (m->nanoseconds() < 500000000) {
            pen.pump(*m);
            if (m->adc.stylusDown() != was) {
                was = m->adc.stylusDown();
                seen.push_back({was, m->nanoseconds()});
            }
            m->run(1000000);  // about 1 ms at the reset clock
        }
        assert(seen.size() == 4 && pen.empty());
        assert(seen[0].first && !seen[1].first && seen[2].first && !seen[3].first);
        assert(seen[1].second - seen[0].second >= S3cPenPacer::holdNs);
        assert(seen[2].second - seen[1].second >= S3cPenPacer::gapNs);
        assert(seen[3].second - seen[2].second >= S3cPenPacer::holdNs);
        assert(seen[1].second - seen[0].second < S3cPenPacer::holdNs + 5000000);
    }
    // A host tap: a 1 ms tap, both events taken in one pump, still holds the pen holdNs of guest time (WM5's
    // alignment needs about 100 ms); a 300 ms host hold is kept as 300 ms of guest time however little guest
    // time passed meanwhile; a 5 s hold is kept to hostHoldLimitNs.
    {
        auto run = [&](uint64_t hostHeldNs) {
            auto m = make({loop});
            S3cPenPacer pen;
            pen.clear(*m);
            pen.push(true, 300, 400, 7000000000);
            pen.push(false, 0, 0, 7000000000 + hostHeldNs);
            uint64_t downAt = 0, upAt = 0;
            bool was = false;
            while (m->nanoseconds() < 2000000000 && !upAt) {
                pen.pump(*m);
                if (m->adc.stylusDown() != was) {
                    was = m->adc.stylusDown();
                    (was ? downAt : upAt) = m->nanoseconds();
                }
                m->run(1000000);
            }
            assert(upAt > downAt && pen.empty());
            return upAt - downAt;
        };
        const uint64_t tap = run(1000000), held = run(300000000), long_ = run(5000000000);
        assert(tap >= S3cPenPacer::holdNs && tap >= 100000000 && tap < S3cPenPacer::holdNs + 5000000);
        assert(held >= 300000000 && held < 305000000);
        assert(long_ >= S3cPenPacer::hostHoldLimitNs && long_ < S3cPenPacer::hostHoldLimitNs + 5000000);
    }
    // Key pacing: a key's down and up queued together reach its pin at least holdNs of guest time apart,
    // its next down waits gapNs after the up, and another key's events behind it wait their turn.
    {
        auto m = make({loop});
        S3cKeyPacer keys;
        keys.clear();
        keys.push(S3cGpio::g, 1, true);
        keys.push(S3cGpio::g, 1, false);
        keys.push(S3cGpio::g, 1, true);
        keys.push(S3cGpio::g, 1, false);
        keys.push(S3cGpio::f, 2, true);
        keys.push(S3cGpio::f, 2, false);
        std::vector<std::pair<bool, uint64_t>> seenG, seenF;
        bool wasG = false, wasF = false;
        while (m->nanoseconds() < 500000000) {
            keys.pump(*m);
            const bool g = !(m->gpio.input[S3cGpio::g] & 2), f = !(m->gpio.input[S3cGpio::f] & 4);
            if (g != wasG) seenG.push_back({wasG = g, m->nanoseconds()});
            if (f != wasF) seenF.push_back({wasF = f, m->nanoseconds()});
            m->run(1000000);
        }
        assert(seenG.size() == 4 && seenF.size() == 2 && keys.empty());
        assert(seenG[0].first && !seenG[1].first && seenG[2].first && !seenG[3].first);
        assert(seenG[1].second - seenG[0].second >= S3cKeyPacer::holdNs);
        assert(seenG[1].second - seenG[0].second < S3cKeyPacer::holdNs + 5000000);
        assert(seenG[2].second - seenG[1].second >= S3cKeyPacer::gapNs);
        assert(seenG[3].second - seenG[2].second >= S3cKeyPacer::holdNs);
        assert(seenF[0].second >= seenG[3].second);
        assert(seenF[1].second - seenF[0].second >= S3cKeyPacer::holdNs);
    }
    // A jam and the host on one key: the pin stays low while either holds it, so the end of a jam doesn't
    // let go of a key the host still holds, and the host's up doesn't end a jam.
    {
        auto m = make({loop});
        S3cKeyPacer keys;
        keys.clear();
        auto low = [&] { return !(m->gpio.input[S3cGpio::g] & 2); };
        m->run(30000000);  // past gapNs, so the host's first down is due at once
        keys.jam(*m, S3cGpio::g, 1, true);
        assert(low());
        keys.push(S3cGpio::g, 1, true);
        keys.pump(*m);
        keys.jam(*m, S3cGpio::g, 1, false);
        assert(low());
        m->run(50000000);
        keys.jam(*m, S3cGpio::g, 1, true);
        keys.push(S3cGpio::g, 1, false);
        keys.pump(*m);
        assert(keys.empty() && low());
        keys.jam(*m, S3cGpio::g, 1, false);
        assert(!low());
        // Two jams over each other: the first to end leaves the pin down for the second.
        keys.jam(*m, S3cGpio::g, 1, true);
        keys.jam(*m, S3cGpio::g, 1, true);
        keys.jam(*m, S3cGpio::g, 1, false);
        assert(low());
        keys.jam(*m, S3cGpio::g, 1, false);
        assert(!low());
    }
    std::printf("s3c2442 machine: ok\n");
}
