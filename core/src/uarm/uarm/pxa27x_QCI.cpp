#include "pxa27x_QCI.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "camera_input.h"
#include "cputil.h"
#include "i2cdev_OV9640.h"
#include "savestate/savestateAll.h"

#define PXA_QCI_SIZE 0x1000UL
#define QCI_STATE_VERSION 0

// The interface clock MCLK is derived from: CICLK / (2 * (CICR4.DIV + 1)). Taken as 104 MHz,
// the PXA27x LCD clock at the Zire's 312 MHz setting.
#define QCI_CICLK_HZ 104000000u

enum : uint32_t {
    CICR0_DMAEN = 1u << 31, CICR0_ENB = 1u << 28, CICR0_DIS = 1u << 27,
    CICR4_MCLK_EN = 1u << 19,
    CISR_IFO = 7u << 0, CISR_EOF = 1u << 3, CISR_SOF = 1u << 4, CISR_CDD = 1u << 5,
    CISR_CQD = 1u << 6, CISR_PAR = 1u << 7, CISR_EOL = 1u << 8, CISR_FEMPTY = 7u << 9,
    CISR_RDAV = 7u << 12, CISR_FTO = 1u << 15,
    CISR_STICKY = CISR_IFO | CISR_EOF | CISR_SOF | CISR_CDD | CISR_CQD | CISR_PAR | CISR_EOL | CISR_FTO,
    CIFR_RESET_F = 1u << 3,
};

// FIFO 0 holds 128 entries of 8 bytes, FIFOs 1 and 2 hold 64; CIFR.FLVLn counts entries.
static const uint16_t qciFifoBytes[3] = {1024, 512, 512};
static const uint8_t qciDmaLine[3] = {DMA_CMR_QCIF_RX_0, DMA_CMR_QCIF_RX_1, DMA_CMR_QCIF_RX_2};

struct QciFifo {
    uint8_t data[1024];
    uint16_t head, count;
};

struct PxaQci {
    PxaDma *dma;
    PxaIc *ic;
    Ov9640 *sensor;
    GetEmuTime getTime;
    Reschedule reschedule;

    uint32_t cicr[5], cisr, cifr, citor;
    QciFifo fifo[3];

    // A frame runs from SOF at frameStart for activeNs, delivering each plane's bytes evenly
    // over that time, then EOF; the next SOF is periodNs after this one. Times are guest ns.
    bool running, inFrame, disablePending;
    uint64_t nextSof, frameStart, frameNumber;
    uint32_t activeNs, periodNs;
    // The frame being delivered, fixed at SOF: QCI window, CICR1 and the sensor bus format.
    uint32_t frameWidth, frameHeight, frameCicr1;
    uint8_t frameFormat, frameOrder;
    uint32_t planeBytes[3], pushed[3], lines;

    // Not session state: the frame's plane bytes (rebuilt from the frame number after a load)
    // and the host source.
    std::vector<uint8_t> plane[3];
    std::vector<uint8_t> rgb;
    bool planesValid;
    CameraSource source;
    unsigned fences;
    bool warned;
};

static void pxaQciPrvReset(PxaQci *q);

static uint64_t pxaQciPrvNow(PxaQci *q) { return q->getTime.getTimeF(q->getTime.userdata); }

static uint32_t pxaQciPrvThreshold(const PxaQci *q, unsigned channel) {
    // CIFR.THL_0 sets FIFO 0's request level at 32, 64 or 96 bytes; FIFOs 1 and 2 ask at 32.
    if (channel) return 32;
    const uint32_t thl = (q->cifr >> 4) & 3;
    return thl >= 2 ? 96 : 32u * (thl + 1);
}

static uint32_t pxaQciPrvLevelBits(const PxaQci *q) {
    uint32_t bits = 0;
    for (unsigned c = 0; c < 3; ++c) {
        if (!q->fifo[c].count) bits |= 1u << (9 + c);
        else if (q->fifo[c].count >= pxaQciPrvThreshold(q, c)) bits |= 1u << (12 + c);
    }
    return bits;
}

static void pxaQciPrvUpdate(PxaQci *q) {
    // CICR0 bits 9:0 mask the CISR sources (set = masked).
    const uint32_t m = q->cicr[0];
    uint32_t enabled = 0;
    if (!(m & (1u << 0))) enabled |= CISR_IFO;
    if (!(m & (1u << 1))) enabled |= CISR_EOF;
    if (!(m & (1u << 2))) enabled |= CISR_SOF;
    if (!(m & (1u << 3))) enabled |= CISR_CDD;
    if (!(m & (1u << 4))) enabled |= CISR_CQD;
    if (!(m & (1u << 5))) enabled |= CISR_PAR;
    if (!(m & (1u << 6))) enabled |= CISR_EOL;
    if (!(m & (1u << 7))) enabled |= CISR_FEMPTY;
    if (!(m & (1u << 8))) enabled |= CISR_RDAV;
    if (!(m & (1u << 9))) enabled |= CISR_FTO;
    pxaIcInt(q->ic, PXA_I_CIF, ((q->cisr | pxaQciPrvLevelBits(q)) & enabled) != 0);
    // A FIFO asks for DMA at its threshold, and for its last bytes once the frame has ended.
    for (unsigned c = 0; c < 3; ++c) {
        const auto count = q->fifo[c].count;
        const bool request = (q->cicr[0] & CICR0_DMAEN) && (q->cifr & (1u << c)) && count &&
                             (count >= pxaQciPrvThreshold(q, c) || !q->inFrame);
        pxaDmaExternalReq(q->dma, qciDmaLine[c], request);
    }
}

static void pxaQciPrvResetFifos(PxaQci *q) {
    for (auto &f : q->fifo) f.head = f.count = 0;
}

static uint32_t pxaQciPrvMclk(const PxaQci *q) {
    if (!(q->cicr[4] & CICR4_MCLK_EN)) return 0;
    return QCI_CICLK_HZ / (2 * ((q->cicr[4] & 0xff) + 1));
}

// The sensor drives pixels only while it runs and has its input clock.
static bool pxaQciPrvSensorClocked(const PxaQci *q) {
    return ov9640Running(q->sensor) && pxaQciPrvMclk(q);
}

// RGB24 to YCbCr, full range BT.601 as in JFIF, in 16.16 fixed point.
static void pxaQciPrvYcc(const uint8_t *p, int &y, int &cb, int &cr) {
    const int r = p[0], g = p[1], b = p[2];
    y = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
    cb = ((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16) + 128;
    cr = ((32768 * r - 27439 * g - 5329 * b + 32768) >> 16) + 128;
    cb = std::clamp(cb, 0, 255);
    cr = std::clamp(cr, 0, 255);
}

// Builds the frame's bus bytes from the picture and splits them into the three planes as
// CICR1 asks: planar YCbCr 4:2:2 (bit 10, COLOR_SP 2) takes Cb, Y, Cr, Y bus order; every
// other mode stores the bus bytes in FIFO 0 as they come.
static void pxaQciPrvRender(PxaQci *q) {
    const uint32_t w = q->frameWidth, h = q->frameHeight;
    q->rgb.resize(size_t(w) * h * 3);
    const bool fenced = q->fences != 0;
    if (fenced || !q->source.pull ||
        !q->source.pull(q->source.context, w, h, q->frameNumber, q->rgb.data()))
        cameraTestPattern(w, h, q->frameNumber, q->rgb.data());
    const bool planar = ((q->frameCicr1 >> 3) & 3) == 2 && (q->frameCicr1 & (1u << 10));
    for (auto &p : q->plane) p.clear();
    q->plane[0].reserve(size_t(w) * h * 2);
    if (planar) { q->plane[1].reserve(size_t(w) * h / 2); q->plane[2].reserve(size_t(w) * h / 2); }
    const auto format = Ov9640Format(q->frameFormat);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row = q->rgb.data() + size_t(y) * w * 3;
        for (uint32_t x = 0; x < w; x += 2) {
            const uint8_t *a = row + x * 3, *b = x + 1 < w ? a + 3 : a;
            uint8_t bus[4];
            if (format == Ov9640Format::yuv422) {
                int y0, y1, cb0, cb1, cr0, cr1;
                pxaQciPrvYcc(a, y0, cb0, cr0);
                pxaQciPrvYcc(b, y1, cb1, cr1);
                const uint8_t u = uint8_t((cb0 + cb1 + 1) / 2), v = uint8_t((cr0 + cr1 + 1) / 2);
                switch (q->frameOrder) {
                    case 0: bus[0] = y0; bus[1] = u; bus[2] = y1; bus[3] = v; break;  // YUYV
                    case 1: bus[0] = y0; bus[1] = v; bus[2] = y1; bus[3] = u; break;  // YVYU
                    case 2: bus[0] = u; bus[1] = y0; bus[2] = v; bus[3] = y1; break;  // UYVY
                    default: bus[0] = v; bus[1] = y0; bus[2] = u; bus[3] = y1; break; // VYUY
                }
            } else {
                // RGB565 or RGB555 (top bit clear), low byte first; raw Bayer sends the green
                // channel as 8-bit samples two to a pixel pair.
                for (unsigned i = 0; i < 2; ++i) {
                    const uint8_t *p = i ? b : a;
                    uint16_t v;
                    if (format == Ov9640Format::rgb555)
                        v = uint16_t(((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
                    else if (format == Ov9640Format::rgb565)
                        v = uint16_t(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                    else
                        v = uint16_t(p[1] | (p[1] << 8));
                    bus[2 * i] = uint8_t(v);
                    bus[2 * i + 1] = uint8_t(v >> 8);
                }
            }
            const unsigned pairBytes = x + 1 < w ? 4 : 2;
            if (planar) {
                q->plane[1].push_back(bus[0]);
                q->plane[0].push_back(bus[1]);
                q->plane[2].push_back(bus[2]);
                if (pairBytes == 4) q->plane[0].push_back(bus[3]);
            } else {
                q->plane[0].insert(q->plane[0].end(), bus, bus + pairBytes);
            }
        }
    }
    q->planesValid = true;
}

static void pxaQciPrvStartFrame(PxaQci *q) {
    const Ov9640Output out = ov9640Output(q->sensor, pxaQciPrvMclk(q));
    q->frameWidth = ((q->cicr[1] >> 15) & 0x7ff) + 1;
    q->frameHeight = (q->cicr[3] & 0x7ff) + 1;
    q->frameCicr1 = q->cicr[1];
    q->frameFormat = uint8_t(out.format);
    q->frameOrder = out.yuvOrder;
    q->periodNs = out.frameNs;
    // Lines arrive over seven eighths of the frame; the rest is vertical blanking.
    q->activeNs = out.frameNs / 8 * 7;
    q->frameStart = q->nextSof;
    q->inFrame = true;
    q->lines = 0;
    pxaQciPrvRender(q);
    for (unsigned c = 0; c < 3; ++c) {
        q->planeBytes[c] = uint32_t(q->plane[c].size());
        q->pushed[c] = 0;
    }
    q->cisr |= CISR_SOF;
}

static void pxaQciPrvEndFrame(PxaQci *q, uint64_t now) {
    q->inFrame = false;
    q->cisr |= CISR_EOF;
    ++q->frameNumber;
    // After a stall (sensor unclocked) the next SOF is now, not a run of missed frame times.
    q->nextSof = std::max(q->frameStart + q->periodNs, now);
    // Bytes left below the request level wait for a timeout when CITOR is set.
    if (q->citor)
        for (unsigned c = 0; c < 3; ++c)
            if (q->fifo[c].count && q->fifo[c].count < pxaQciPrvThreshold(q, c)) q->cisr |= CISR_FTO;
    if (q->disablePending) {
        q->disablePending = false;
        q->running = false;
        q->cicr[0] &= ~(CICR0_ENB | CICR0_DIS);
        q->cisr |= CISR_CDD;
    }
}

static void pxaQciPrvDeliver(PxaQci *q, uint64_t now) {
    if (!q->planesValid) pxaQciPrvRender(q);
    const uint64_t elapsed = std::min<uint64_t>(now - q->frameStart, q->activeNs);
    for (unsigned c = 0; c < 3; ++c) {
        const uint32_t total = std::min<uint32_t>(q->planeBytes[c], uint32_t(q->plane[c].size()));
        if (!total) continue;
        const uint32_t due = q->activeNs ? uint32_t(uint64_t(total) * elapsed / q->activeNs) & ~1u : total;
        const uint32_t target = elapsed >= q->activeNs ? total : std::min(due, total);
        if (target <= q->pushed[c]) continue;
        uint32_t n = target - q->pushed[c];
        QciFifo &f = q->fifo[c];
        if (q->cifr & (1u << c)) {
            const uint32_t space = qciFifoBytes[c] - f.count;
            if (n > space) q->cisr |= 1u << c;  // overrun: the rest of these bytes are lost
            const uint32_t take = std::min(n, space);
            const uint8_t *src = q->plane[c].data() + q->pushed[c];
            for (uint32_t i = 0; i < take; ++i)
                f.data[(f.head + f.count + i) % qciFifoBytes[c]] = src[i];
            f.count += take;
        }
        q->pushed[c] += n;
    }
    // EOL as each line of FIFO 0's plane completes.
    const uint32_t lineBytes = q->frameHeight ? q->planeBytes[0] / q->frameHeight : 0;
    const uint32_t lines = lineBytes ? q->pushed[0] / lineBytes : 0;
    if (lines > q->lines) { q->lines = lines; q->cisr |= CISR_EOL; }
    if (elapsed >= q->activeNs) pxaQciPrvEndFrame(q, now);
}

void pxaQciPeriodic(PxaQci *q) {
    if (!q->running) return;
    if (!pxaQciPrvSensorClocked(q)) return;  // no pixel clock: the frame stalls
    const uint64_t now = pxaQciPrvNow(q);
    if (!q->inFrame && now >= q->nextSof) {
        q->nextSof = now;  // a stalled sensor does not queue the frames it missed
        pxaQciPrvStartFrame(q);
    }
    if (q->inFrame) pxaQciPrvDeliver(q, now);
    // A long gap ends this frame and starts the next one at its own time on the next pass.
    pxaQciPrvUpdate(q);
}

// Polled while enabled, so a sensor coming out of power-down or a clock turning on is seen.
bool pxaQciTaskRequired(PxaQci *q) { return q->running; }

static void pxaQciPrvWriteCicr0(PxaQci *q, uint32_t value) {
    const uint32_t old = q->cicr[0];
    q->cicr[0] = value;
    if ((value & CICR0_ENB) && !(old & CICR0_ENB)) {
        // Enable: the first frame starts after one blanking interval.
        q->running = true;
        q->disablePending = false;
        q->inFrame = false;
        const Ov9640Output out = ov9640Output(q->sensor, pxaQciPrvMclk(q));
        q->nextSof = pxaQciPrvNow(q) + out.frameNs / 8;
        q->reschedule.rescheduleCb(q->reschedule.ctx, RESCHEDULE_TASK_DEVICE_TIER0);
    } else if (!(value & CICR0_ENB) && (old & CICR0_ENB)) {
        // Quick disable: stops at once, mid frame or not.
        q->running = q->inFrame = q->disablePending = false;
        q->cisr |= CISR_CQD;
    } else if ((value & CICR0_DIS) && (value & CICR0_ENB) && q->running) {
        // Disable: finishes the frame being captured first.
        if (q->inFrame) q->disablePending = true;
        else {
            q->running = false;
            q->cicr[0] &= ~(CICR0_ENB | CICR0_DIS);
            q->cisr |= CISR_CDD;
        }
    }
}

static uint32_t pxaQciPrvPop(PxaQci *q, unsigned c, unsigned size) {
    QciFifo &f = q->fifo[c];
    uint32_t value = 0;
    for (unsigned i = 0; i < size && f.count; ++i) {
        value |= uint32_t(f.data[f.head]) << (8 * i);
        f.head = uint16_t((f.head + 1) % qciFifoBytes[c]);
        --f.count;
    }
    return value;
}

static bool pxaQciPrvAccess(void *userData, uint32_t pa, uint_fast8_t size, bool write, void *buf) {
    auto *q = static_cast<PxaQci *>(userData);
    const uint32_t offset = pa - PXA_QCI_BASE;
    // The receive buffers take byte and halfword reads for a DMA tail; registers are words.
    if (!write && (offset == 0x28 || offset == 0x30 || offset == 0x38)) {
        const uint32_t value = pxaQciPrvPop(q, (offset - 0x28) / 8, size);
        if (size == 4) *static_cast<uint32_t *>(buf) = value;
        else if (size == 2) *static_cast<uint16_t *>(buf) = uint16_t(value);
        else *static_cast<uint8_t *>(buf) = uint8_t(value);
        pxaQciPrvUpdate(q);
        return true;
    }
    if (size != 4) return false;
    uint32_t value = write ? *static_cast<uint32_t *>(buf) : 0;
    switch (offset) {
        case 0x00:
            if (write) pxaQciPrvWriteCicr0(q, value);
            else value = q->cicr[0];
            break;
        case 0x04: case 0x08: case 0x0c: case 0x10:
            if (write) q->cicr[offset / 4] = value;
            else value = q->cicr[offset / 4];
            break;
        case 0x14:
            if (write) q->cisr &= ~(value & CISR_STICKY);
            else value = q->cisr | pxaQciPrvLevelBits(q);
            break;
        case 0x18:
            if (write) {
                q->cifr = value & 0x37;
                if (value & CIFR_RESET_F) pxaQciPrvResetFifos(q);
            } else {
                // FLVL0 is bits 15:8 (0xff), FLVL1 and FLVL2 are 7 bits; a full FIFO 0 reads 128.
                value = q->cifr | uint32_t(q->fifo[0].count / 8) << 8 |
                        uint32_t(q->fifo[1].count / 8) << 16 | uint32_t(q->fifo[2].count / 8) << 23;
            }
            break;
        case 0x1c:
            if (write) q->citor = value;
            else value = q->citor;
            break;
        case 0x28: case 0x30: case 0x38:
            break;  // receive buffers ignore writes
        default:
            if (!write) value = 0;
            break;
    }
    if (!write) *static_cast<uint32_t *>(buf) = value;
    pxaQciPrvUpdate(q);
    return true;
}

static uint_fast8_t pxaQciPrvValidBytes(void *ctx, unsigned c) {
    return uint_fast8_t(std::min<uint32_t>(static_cast<PxaQci *>(ctx)->fifo[c].count, 0xfc));
}

PxaQci *pxaQciInit(ArmMem *physMem, PxaIc *ic, PxaDma *dma, Ov9640 *sensor, GetEmuTime getTime,
                   Reschedule reschedule) {
    auto *q = new PxaQci{};
    q->dma = dma;
    q->ic = ic;
    q->sensor = sensor;
    q->getTime = getTime;
    q->reschedule = reschedule;
    pxaQciPrvReset(q);
    if (!memRegionAdd(physMem, PXA_QCI_BASE, PXA_QCI_SIZE, pxaQciPrvAccess, q))
        ERR("cannot add QCI to MEM");
    static uint_fast8_t (*const valid[3])(void *) = {
        [](void *c) { return pxaQciPrvValidBytes(c, 0); },
        [](void *c) { return pxaQciPrvValidBytes(c, 1); },
        [](void *c) { return pxaQciPrvValidBytes(c, 2); },
    };
    for (unsigned c = 0; c < 3; ++c) {
        PxaDmaPeripheral fifo{};
        fifo.validBytes = valid[c];
        fifo.ctx = q;
        fifo.drain = true;
        pxaDmaSetPeripheral(dma, qciDmaLine[c], &fifo);
    }
    return q;
}

void pxaQciDestroy(PxaQci *q) {
    if (!q) return;
    for (unsigned c = 0; c < 3; ++c) pxaDmaSetPeripheral(q->dma, qciDmaLine[c], nullptr);
    pxaQciDisarmSource(q);
    delete q;
}

void pxaQciDisarmSource(PxaQci *q) {
    if (q->source.disarm) q->source.disarm(q->source.context);
    q->source = {};
}

void pxaQciSetSource(PxaQci *q, const CameraSource *source) {
    pxaQciDisarmSource(q);
    if (source) q->source = *source;
    if (q->fences && q->source.fence) q->source.fence(q->source.context, true);
}

void pxaQciFenceSource(PxaQci *q, bool fence) {
    const unsigned before = q->fences;
    if (fence) ++q->fences;
    else if (q->fences) --q->fences;
    if (bool(before) != bool(q->fences) && q->source.fence)
        q->source.fence(q->source.context, q->fences != 0);
}

bool pxaQciCapturing(const PxaQci *q) { return q->running && pxaQciPrvSensorClocked(q); }
uint64_t pxaQciFrameNumber(const PxaQci *q) { return q->frameNumber; }

template <typename H> static void pxaQciPrvState(PxaQci &q, H &h) {
    for (auto &r : q.cicr) h.Do32(r);
    h.Do32(q.cisr).Do32(q.cifr).Do32(q.citor);
    for (auto &f : q.fifo) h.Do(typename H::Pack16() << f.head << f.count).DoBuffer(f.data, sizeof(f.data));
    h.Do(typename H::BoolPack() << q.running << q.inFrame << q.disablePending)
        .Do64(q.nextSof).Do64(q.frameStart).Do64(q.frameNumber)
        .Do32(q.activeNs).Do32(q.periodNs)
        .Do32(q.frameWidth).Do32(q.frameHeight).Do32(q.frameCicr1)
        .Do(typename H::Pack8() << q.frameFormat << q.frameOrder);
    for (unsigned c = 0; c < 3; ++c) h.Do32(q.planeBytes[c]).Do32(q.pushed[c]);
    h.Do32(q.lines);
}

template <typename T> void pxaQciSave(PxaQci *q, T &savestate) {
    auto *chunk = savestate.GetChunk(ChunkType::pxa27xQci, QCI_STATE_VERSION);
    if (!chunk) ERR("cannot allocate QCI state");
    SaveChunkHelper h(*chunk);
    pxaQciPrvState(*q, h);
}

void pxaQciReset(PxaQci *q) {
    pxaQciPrvReset(q);
    pxaQciPrvUpdate(q);
}

static void pxaQciPrvReset(PxaQci *q) {
    memset(q->cicr, 0, sizeof(q->cicr));
    q->cicr[0] = 0x3ff;
    q->cisr = q->cifr = q->citor = 0;
    pxaQciPrvResetFifos(q);
    q->running = q->inFrame = q->disablePending = false;
    q->nextSof = q->frameStart = q->frameNumber = 0;
    q->activeNs = q->periodNs = q->frameWidth = q->frameHeight = q->frameCicr1 = 0;
    q->frameFormat = q->frameOrder = 0;
    for (unsigned c = 0; c < 3; ++c) q->planeBytes[c] = q->pushed[c] = 0;
    q->lines = 0;
    q->planesValid = false;
}

template <typename T> bool pxaQciLoad(PxaQci *q, T &loader) {
    if (!loader.HasChunk(ChunkType::pxa27xQci)) {
        pxaQciPrvReset(q);
        pxaQciPrvUpdate(q);
        return true;
    }
    auto *chunk = loader.GetChunkOrFail(ChunkType::pxa27xQci, QCI_STATE_VERSION, "PXA27x QCI");
    if (!chunk) return false;
    PxaQci next;
    next.dma = q->dma; next.ic = q->ic; next.sensor = q->sensor; next.getTime = q->getTime;
    next.reschedule = q->reschedule;
    LoadChunkHelper h(*chunk);
    pxaQciPrvState(next, h);
    bool ok = !chunk->HasError() && next.frameWidth <= 2048 && next.frameHeight <= 2048 &&
              next.frameFormat <= uint8_t(Ov9640Format::rawBayer) && next.frameOrder <= 3 &&
              (!next.inFrame || (next.activeNs && next.frameWidth && next.frameHeight));
    // Plane sizes follow from the window and mode as pxaQciPrvRender builds them: planar 4:2:2
    // is w*h Y bytes and h*ceil(w/2) for each of Cb and Cr, anything else is w*h*2 in FIFO 0.
    // Between frames the sizes may also be all zero.
    const bool planar = ((next.frameCicr1 >> 3) & 3) == 2 && (next.frameCicr1 & (1u << 10));
    const uint32_t pixels = next.frameWidth * next.frameHeight;
    const uint32_t chroma = next.frameHeight * ((next.frameWidth + 1) / 2);
    const uint32_t expect[3] = {planar ? pixels : pixels * 2, planar ? chroma : 0, planar ? chroma : 0};
    const bool idle = !next.planeBytes[0] && !next.planeBytes[1] && !next.planeBytes[2];
    for (unsigned c = 0; c < 3 && ok; ++c)
        ok = next.fifo[c].head < qciFifoBytes[c] && next.fifo[c].count <= qciFifoBytes[c] &&
             (next.planeBytes[c] == expect[c] || (idle && !next.inFrame)) && next.pushed[c] <= next.planeBytes[c];
    if (!ok) { loader.NotifyError(); return false; }
    memcpy(q->cicr, next.cicr, sizeof(q->cicr));
    q->cisr = next.cisr; q->cifr = next.cifr; q->citor = next.citor;
    memcpy(q->fifo, next.fifo, sizeof(q->fifo));
    q->running = next.running; q->inFrame = next.inFrame; q->disablePending = next.disablePending;
    q->nextSof = next.nextSof; q->frameStart = next.frameStart; q->frameNumber = next.frameNumber;
    q->activeNs = next.activeNs; q->periodNs = next.periodNs;
    q->frameWidth = next.frameWidth; q->frameHeight = next.frameHeight; q->frameCicr1 = next.frameCicr1;
    q->frameFormat = next.frameFormat; q->frameOrder = next.frameOrder;
    memcpy(q->planeBytes, next.planeBytes, sizeof(q->planeBytes));
    memcpy(q->pushed, next.pushed, sizeof(q->pushed));
    q->lines = next.lines;
    // The frame in flight is drawn again from its number, with no host source.
    q->planesValid = false;
    if (q->running) q->reschedule.rescheduleCb(q->reschedule.ctx, RESCHEDULE_TASK_DEVICE_TIER0);
    pxaQciPrvUpdate(q);
    return true;
}

template void pxaQciSave<Savestate<ChunkType>>(PxaQci *, Savestate<ChunkType> &);
template void pxaQciSave<SavestateProbe<ChunkType>>(PxaQci *, SavestateProbe<ChunkType> &);
template bool pxaQciLoad<SavestateLoader<ChunkType>>(PxaQci *, SavestateLoader<ChunkType> &);
