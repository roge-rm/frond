// Firmware-free Zire 72 camera: the OV9640 on the standard I2C (SCCB) and the PXA27x quick
// capture interface through the real SoC. Covers the sensor's power, reset and power-down
// gating, the test pattern, FIFOs, the CIBR reads, a DMA descriptor chain filling the three
// planes, interrupts, saved state and the source lifecycle.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "soc_PXA.h"
#include "camera_input.h"
#include "mem.h"

namespace {
constexpr uint32_t I2C = 0x40301680, IDBR = I2C + 8, ICR = I2C + 0x10, ISR = I2C + 0x18;
constexpr uint32_t GPIO = 0x40e00000;
constexpr uint32_t QCI = 0x50000000, CICR0 = QCI, CICR1 = QCI + 4, CICR3 = QCI + 0xc,
                   CICR4 = QCI + 0x10, CISR = QCI + 0x14, CIFR = QCI + 0x18, CIBR0 = QCI + 0x28;
constexpr uint32_t DMA = 0x40000000;
// The Zire's CameraDriver values: QVGA planar YCbCr 4:2:2, 26 MHz MCLK.
constexpr uint32_t ROM_CICR1 = 0x009f8412, ROM_CICR3 = 0xef, ROM_CICR4 = 0x880001;
constexpr unsigned W = 320, H = 240;

struct Machine {
    std::vector<uint8_t> rom;
    SocPXA soc;
    Machine() : rom(4096), soc(deviceTypeZire72, 32u << 20, rom.data(), uint32_t(rom.size()), nullptr, 0, -1, 2) {
        wr(0xa0003000, 0xeafffffe);
        cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
        cpuSetReg(soc.GetCpu(), 15, 0xa0003000);
    }
    void wr(uint32_t a, uint32_t v) { assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, true, &v)); }
    uint32_t rd(uint32_t a) { uint32_t v = 0; assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, false, &v)); return v; }
    uint8_t rd8(uint32_t a) { uint8_t v = 0; assert(memAccess(cpuGetMem(soc.GetCpu()), a, 1, false, &v)); return v; }
    void run(unsigned ms) { soc.Run(uint64_t(ms) * 100000, 100000000); }

    // GPIO 91 is the supply (low on), 57 RESET and 56 PWDN (high active), all outputs.
    void pins(bool supplyOn, bool reset, bool powerDown) {
        wr(GPIO + 0x10, rd(GPIO + 0x10) | 3u << 24);
        wr(GPIO + 0x14, rd(GPIO + 0x14) | 1u << 27);
        wr(GPIO + (supplyOn ? 0x2c : 0x20), 1u << 27);
        wr(GPIO + (reset ? 0x1c : 0x28), 1u << 25);
        wr(GPIO + (powerDown ? 0x1c : 0x28), 1u << 24);
    }
    // One SCCB byte with ICR bits on top of IUE and TB; true when the slave acked.
    bool send(uint8_t byte, uint32_t extra) {
        wr(IDBR, byte);
        wr(ICR, 0x48 | extra);
        return !(rd(ISR) & 2);
    }
    bool sccbWrite(uint8_t reg, uint8_t value) {
        if (!send(0x60, 1)) { wr(ICR, 0x50); return false; }
        send(reg, 0);
        send(value, 2);
        return true;
    }
    // Write phase for the sub-address, then a read phase, as the ROM's probe does.
    int sccbRead(uint8_t reg) {
        if (!send(0x60, 1)) { wr(ICR, 0x50); return -1; }
        send(reg, 2);
        wr(ICR, 0x40);
        if (!send(0x61, 1)) { wr(ICR, 0x50); return -1; }
        wr(ICR, 0x48 | 4 | 2);
        const int value = int(rd(IDBR) & 0xff);
        wr(ISR, 0x80);  // IRF
        return value;
    }
    void sensorQvga() {
        pins(true, false, false);
        assert(sccbWrite(0x12, 0x80));
        assert(sccbWrite(0x12, 0x10));  // COM7: QVGA YUV
        assert(sccbWrite(0x11, 0x83));  // CLKRC
        assert(sccbWrite(0x3a, 0x08));  // TSLB: UYVY
    }
    void qciRom(uint32_t cicr0) {
        wr(CIFR, 0x0f);  // FIFOs on and reset
        wr(CICR4, ROM_CICR4);
        wr(CICR1, ROM_CICR1);
        wr(CICR3, ROM_CICR3);
        wr(CICR0, cicr0);
    }
    std::vector<uint8_t> save() {
        assert(soc.Save());
        auto d = soc.GetSavestate();
        return {(uint8_t *)d.data, (uint8_t *)d.data + d.size};
    }
};

// The planes the QCI should deliver for one frame of the pattern, by the JFIF formula.
struct Planes {
    std::vector<uint8_t> y, cb, cr;
    explicit Planes(uint64_t frame) {
        std::vector<uint8_t> rgb(W * H * 3);
        cameraTestPattern(W, H, frame, rgb.data());
        for (unsigned i = 0; i < W * H; i += 2) {
            int u = 0, v = 0;
            for (unsigned k = 0; k < 2; ++k) {
                const uint8_t *p = &rgb[(i + k) * 3];
                y.push_back(uint8_t((19595 * p[0] + 38470 * p[1] + 7471 * p[2] + 32768) >> 16));
                u += std::clamp(((-11059 * p[0] - 21709 * p[1] + 32768 * p[2] + 32768) >> 16) + 128, 0, 255);
                v += std::clamp(((32768 * p[0] - 27439 * p[1] - 5329 * p[2] + 32768) >> 16) + 128, 0, 255);
            }
            cb.push_back(uint8_t((u + 1) / 2));
            cr.push_back(uint8_t((v + 1) / 2));
        }
    }
};

// A descriptor chain moving bytes from one receive buffer to dest in 8000-byte pieces, with
// the ROM's DCMD (target increments, flow from the source, 32-byte bursts, width 0); the last
// descriptor stops the channel.
void chain(Machine &m, unsigned channel, uint32_t descriptors, uint32_t dest, uint32_t bytes) {
    const uint32_t source = CIBR0 + 8 * channel;
    for (uint32_t done = 0, at = descriptors; done < bytes; done += 8000, at += 16) {
        const uint32_t n = std::min<uint32_t>(8000, bytes - done);
        m.wr(at, done + n < bytes ? at + 16 : 1);
        m.wr(at + 4, source);
        m.wr(at + 8, dest + done);
        m.wr(at + 12, 0x60030000 | n);
    }
    m.wr(DMA + 0x1110 + 4 * channel, 0x80 | channel);  // DRCMR68 + channel
    m.wr(DMA + 0x200 + 16 * channel, descriptors);       // DDADR
    m.wr(DMA + 4 * channel, 0x80000000);                 // DCSR RUN
}

bool equal(Machine &m, uint32_t at, const std::vector<uint8_t> &want) {
    for (size_t i = 0; i < want.size(); ++i)
        if (m.rd8(at + uint32_t(i)) != want[i]) {
            printf("byte %zu at %08x: %u, want %u\n", i, at, m.rd8(at + uint32_t(i)), want[i]);
            return false;
        }
    return true;
}

// The payload of the saved chunk of this type: its first word is the chunk version.
uint32_t *chunkPayload(std::vector<uint8_t> &saved, uint32_t type) {
    auto *words = reinterpret_cast<uint32_t *>(saved.data());
    unsigned offset = 1 + 2 * words[0];
    for (unsigned i = 0; i < words[0]; ++i) {
        if (words[1 + 2 * i] == type) return words + offset;
        offset += words[2 + 2 * i];
    }
    return nullptr;
}

struct Source {
    unsigned pulls{}, fences{}, disarms{};
    bool fenced{};
    CameraSource Hooks() {
        return {this,
                [](void *p, uint32_t w, uint32_t h, uint64_t, uint8_t *rgb) {
                    auto &s = *static_cast<Source *>(p);
                    assert(!s.fenced);
                    ++s.pulls;
                    memset(rgb, 0x80, size_t(w) * h * 3);  // mid grey
                    return true;
                },
                [](void *p, bool f) { auto &s = *static_cast<Source *>(p); s.fenced = f; ++s.fences; },
                [](void *p) { auto &s = *static_cast<Source *>(p); ++s.disarms; s.fenced = false; }};
    }
};
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // The pattern: bars, the moving bar's place and the counter's first digit cell.
    {
        std::vector<uint8_t> rgb(W * H * 3);
        cameraTestPattern(W, H, 5, rgb.data());
        for (unsigned bar = 0; bar < 8; ++bar)
            assert(!memcmp(&rgb[(10 * W + bar * 40 + 20) * 3], cameraTestPatternBars[bar], 3));
        const unsigned band = 175 * W * 3;
        assert(rgb[band + (5 * W / 64) * 3] == 255 && rgb[band + 0] == 64);
        assert(rgb[(H - 1) * W * 3] == 0);
    }

    // SCCB answers at 0x30 only with the supply on, RESET low and PWDN low.
    {
        Machine m;
        m.pins(false, false, false);
        assert(m.sccbRead(0x0a) == -1);
        m.pins(true, true, false);
        assert(m.sccbRead(0x0a) == -1);
        m.pins(true, false, true);
        assert(m.sccbRead(0x0a) == -1);
        m.pins(true, false, false);
        assert(m.sccbRead(0x0a) == 0x96 && m.sccbRead(0x0b) == 0x48);
        assert(m.sccbRead(0x1c) == 0x7f && m.sccbRead(0x1d) == 0xa2);
        // Registers keep writes, the ID doesn't; PWDN keeps them, a RESET pulse doesn't.
        assert(m.sccbWrite(0x11, 0x83) && m.sccbWrite(0x0a, 0) && m.sccbRead(0x0a) == 0x96);
        m.pins(true, false, true);
        m.pins(true, false, false);
        assert(m.sccbRead(0x11) == 0x83);
        m.pins(true, true, false);
        m.pins(true, false, false);
        assert(m.sccbRead(0x11) == 0);
        // COM7 bit 7 resets the registers too.
        assert(m.sccbWrite(0x3a, 0x08) && m.sccbWrite(0x12, 0x80) && m.sccbRead(0x3a) == 0x0c);
        // Another address on the bus gets no ack.
        assert(!m.send(0x42, 1));
        m.wr(ICR, 0x50);
    }

    // Without DMA the FIFOs fill and overrun; CIBR0 pops the frame's first Y bytes; EOF and
    // SOF raise IRQ 33 when unmasked. Nothing comes without MCLK or with the sensor off.
    {
        Machine m;
        m.sensorQvga();
        m.wr(CIFR, 0x0f);
        m.wr(CICR1, ROM_CICR1);
        m.wr(CICR3, ROM_CICR3);
        m.wr(CICR0, 0x100003ff);  // ENB, all masked, no MCLK
        m.run(300);
        assert(!(m.rd(CISR) & 0x18) && (m.rd(CISR) & 0x0e00) == 0x0e00);  // FIFOs empty
        m.wr(CICR0, 0);
        m.qciRom(0x100003f9);  // ENB with SOF and EOF unmasked
        m.run(300);
        const uint32_t cisr = m.rd(CISR), cifr = m.rd(CIFR);
        printf("no DMA: CISR %08x CIFR %08x\n", cisr, cifr);
        assert((cisr & 0x18) == 0x18 && (cisr & 7) == 7);  // SOF, EOF, all three overran
        assert(((cifr >> 8) & 0xff) == 128 && ((cifr >> 16) & 0x7f) == 64 && ((cifr >> 23) & 0x7f) == 64);
        assert(m.rd(0x40d000ac) & 2);  // ICPR2: IRQ 33
        m.wr(CISR, 0x18 | 7);
        Planes p(0);
        const uint32_t first = m.rd(CIBR0);
        assert(first == uint32_t(p.y[0] | p.y[1] << 8 | p.y[2] << 16 | p.y[3] << 24));
        assert(((m.rd(CIFR) >> 8) & 0xff) == 127);  // 1020 bytes
        m.wr(CIFR, 0x0f);  // RESET_F empties them
        assert(((m.rd(CIFR) >> 8) & 0xff) == 0 && (m.rd(CISR) & 0x0e00) == 0x0e00);
        // Sensor powered down: the frame stalls.
        m.pins(true, false, true);
        m.wr(CISR, 0xffff);
        m.run(300);
        assert(!(m.rd(CISR) & 0x18));
        // Quick disable sets CQD.
        m.wr(CICR0, 0x000003ff);
        assert(m.rd(CISR) & 0x40);
    }

    // DMA: a synthetic descriptor chain per plane takes one whole frame, byte for byte.
    {
        Machine m;
        m.sensorQvga();
        chain(m, 0, 0xa0100000, 0xa0200000, W * H);
        chain(m, 1, 0xa0101000, 0xa0220000, W * H / 2);
        chain(m, 2, 0xa0102000, 0xa0240000, W * H / 2);
        m.qciRom(0x900003ff);  // DMAEN, ENB
        // 1 ms steps: QVGA blanking is under 8 ms, so a coarser step lets the next frame overrun.
        for (unsigned i = 0; i < 1000 && (m.rd(DMA) & 8) == 0; ++i) m.run(1);
        for (unsigned c = 0; c < 3; ++c) {
            printf("DCSR%u %08x\n", c, m.rd(DMA + 4 * c));
            assert((m.rd(DMA + 4 * c) & 9) == 8);  // stopped, no bus error
        }
        Planes p(0);
        assert(equal(m, 0xa0200000, p.y) && equal(m, 0xa0220000, p.cb) && equal(m, 0xa0240000, p.cr));
        assert(!(m.rd(CISR) & 7));  // no overrun while DMA kept up

        // Save mid-frame, run on, load and run the same span: the same bytes again. The SoC
        // state leaves RAM to the session, so the buffer is put back by hand.
        chain(m, 0, 0xa0100000, 0xa0300000, W * H * 2);
        m.run(40);
        auto saved = m.save();
        std::vector<uint32_t> atSave(W * H / 2);
        for (size_t i = 0; i < atSave.size(); ++i) atSave[i] = m.rd(0xa0300000 + uint32_t(i) * 4);
        m.run(400);
        std::vector<uint8_t> after(W * H * 2);
        for (size_t i = 0; i < after.size(); ++i) after[i] = m.rd8(0xa0300000 + uint32_t(i));
        assert(after != std::vector<uint8_t>(after.size(), 0));
        for (size_t i = 0; i < atSave.size(); ++i) m.wr(0xa0300000 + uint32_t(i) * 4, atSave[i]);
        assert(m.soc.Load(saved.size(), saved.data()));
        m.run(400);
        assert(equal(m, 0xa0300000, after));

        // A source feeds the frames; checkpoints fence it, load and sleep reset disarm it.
        Source s;
        auto h = s.Hooks();
        m.soc.SetCameraSource(&h);
        m.run(400);
        assert(s.pulls > 0);
        {
            SoC::InputCheckpoint checkpoint(m.soc);
            assert(s.fenced);
            const unsigned pulls = s.pulls;
            m.run(400);
            assert(s.pulls == pulls);
        }
        assert(!s.fenced && s.disarms == 0);
        assert(m.soc.Load(saved.size(), saved.data()));
        assert(s.disarms == 1);
        const unsigned pulls = s.pulls;
        m.run(400);
        assert(s.pulls == pulls);
    }

    // State: a sensor chunk with a wrong ID is refused and the machine carries on as it was.
    {
        Machine m;
        m.sensorQvga();
        m.qciRom(0x100003ff);
        m.run(200);
        auto saved = m.save();
        auto *words = reinterpret_cast<uint32_t *>(saved.data());
        unsigned offset = 1 + 2 * words[0];
        uint32_t *sensor = nullptr;
        for (unsigned i = 0; i < words[0]; ++i) {
            if (words[1 + 2 * i] == 0x0260) sensor = words + offset;
            offset += words[2 + 2 * i];
        }
        assert(sensor);
        auto *regs = reinterpret_cast<uint8_t *>(sensor + 1);
        // The register file follows the flags byte and sub-address; find the PID by value.
        uint8_t *pid = nullptr;
        for (unsigned i = 0; i + 1 < 300 && !pid; ++i)
            if (regs[i] == 0x96 && regs[i + 1] == 0x48) pid = regs + i;
        assert(pid);
        *pid = 0;
        const auto live = m.save();
        assert(!m.soc.Load(saved.size(), saved.data()));
        assert(m.save() == live);
    }

    // A sensor that loses its clock or power while QCI stays enabled picks up with one frame
    // when it comes back, not with the frames it missed.
    for (unsigned how = 0; how < 2; ++how) {
        Machine m;
        Source s;
        auto h = s.Hooks();
        m.soc.SetCameraSource(&h);
        m.sensorQvga();
        m.qciRom(0x100003ff);
        m.run(100);
        assert(s.pulls > 0);
        if (how) m.wr(CICR4, ROM_CICR4 & ~0x80000u);  // MCLK off
        else m.pins(true, false, true);                 // PWDN
        m.run(100);
        const unsigned stalled = s.pulls;
        m.run(2000);
        assert(s.pulls == stalled);
        if (how) m.wr(CICR4, ROM_CICR4);
        else m.pins(true, false, false);
        m.run(20);
        printf("stall %u: %u frames in the first 20 ms\n", how, s.pulls - stalled);
        assert(s.pulls - stalled <= 1);
    }

    // A session claiming planes that don't fit the window and mode is refused, and the
    // machine carries on as it was; the same session left alone loads.
    {
        Machine m;
        m.sensorQvga();
        m.qciRom(0x100003ff);
        m.run(100);
        auto saved = m.save();
        uint32_t *qci = chunkPayload(saved, 0x0270);
        assert(qci);
        // planeBytes and pushed for the three planes follow each other: 76800, n, 38400, n, 38400, n.
        uint32_t *planes = nullptr;
        for (unsigned i = 0; i < 1500 && !planes; ++i)
            if (qci[i] == W * H && qci[i + 2] == W * H / 2 && qci[i + 4] == W * H / 2 && qci[i + 1] <= W * H)
                planes = qci + i;
        assert(planes);
        assert(m.soc.Load(saved.size(), saved.data()));
        const auto live = m.save();
        const uint32_t sizes[][3] = {
            {W * H, 0x7fffffff, W * H / 2}, {W * H, W * H / 2, 0xffffffffu}, {0x40000000, W * H / 2, W * H / 2},
            {W * H, W * H, W * H / 2}, {W * H * 2, 0, 0}, {W * H, 0, 0}, {0, 0, 0}};
        for (const auto &bad : sizes) {
            auto copy = saved;
            uint32_t *p = reinterpret_cast<uint32_t *>(copy.data()) + (planes - reinterpret_cast<uint32_t *>(saved.data()));
            p[0] = bad[0]; p[2] = bad[1]; p[4] = bad[2];
            assert(!m.soc.Load(copy.size(), copy.data()));
        }
        assert(m.save() == live);
        assert(m.soc.Load(saved.size(), saved.data()));
        m.run(100);
    }

    // An LCD chunk from before the overlay registers leaves them at their power-on 0.
    {
        constexpr uint32_t LCD = 0x44000000;
        Machine m;
        for (unsigned i = 0; i < 4; ++i) m.wr(LCD + 0x50 + 0x10 * i, 0x12345678 + i);
        auto saved = m.save();
        assert(m.soc.Load(saved.size(), saved.data()));
        for (unsigned i = 0; i < 4; ++i) assert(m.rd(LCD + 0x50 + 0x10 * i) == 0x12345678 + i);
        for (unsigned version = 0; version < 2; ++version) {
            auto old = saved;
            uint32_t *lcd = chunkPayload(old, 0x0080);
            assert(lcd && lcd[0] == 2);
            lcd[0] = version;
            assert(m.soc.Load(old.size(), old.data()));
            for (unsigned i = 0; i < 4; ++i) assert(m.rd(LCD + 0x50 + 0x10 * i) == 0);
            // Version 2 again brings them back.
            assert(m.soc.Load(saved.size(), saved.data()));
            assert(m.rd(LCD + 0x50) == 0x12345678);
        }
    }

    printf("z72 camera: ok\n");
    return 0;
}
