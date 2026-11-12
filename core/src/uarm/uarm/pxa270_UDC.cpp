//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa270_UDC.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "savestate/savestateAll.h"
#include "mem.h"

#define PXA_UDC_BASE 0x40600000UL
#define PXA_UDC_SIZE 0x00001000UL

// UDCCR (manual 12.6.1)
#define UDCCR_UDE 0x00000001ul
#define UDCCR_UDA 0x00000002ul
#define UDCCR_EMCE 0x00000008ul
#define UDCCR_SMAC 0x00000010ul
#define UDCCR_ACTIVE 0x00001fe0ul  // AAISN 7:5, AIN 10:8, ACN 12:11
#define UDCCR_DRWE 0x00010000ul

// UDCCSRA-X (manual 12.6.8). FS, BNE/BNF and SP are derived from the FIFO on read;
// only DME/FST (R/W) and SST/TRN/PC (W1C) are stored.
#define CSRX_FS 0x001
#define CSRX_PC 0x002
#define CSRX_TRN 0x004
#define CSRX_DME 0x008
#define CSRX_SST 0x010
#define CSRX_FST 0x020
#define CSRX_BNE 0x040
#define CSRX_SP 0x080
#define CSRX_FEF 0x100
#define CSRX_STORED 0x03e

// UDC-answered EP0 request in progress (hw0).
enum : uint8_t { HW0_NONE, HW0_DATA, HW0_STATUS_IN, HW0_STALL, HW0_STATUS_OUT };
#define HW0_GATED 1  // status IN waits for AREN while ACM is set
#define HW0_ADDRESS 2  // the status IN applies pendingAddress

struct Ep {
    uint32_t ccra;  // unused for ep0
    uint16_t csr;
    uint16_t bcr;   // ep0 only; A-X byte counts come from the FIFO
};

// Endpoints A-X: up to two packets of up to 64 bytes (bulk and interrupt). For IN,
// `fill` bytes are being loaded into buffer (head + count) & 1; for OUT, `pos` bytes of
// the head packet have been read.
struct EpFifo {
    uint8_t data[2][64];
    uint8_t size[2];
    uint8_t head, count, fill, pos;
};

struct Pxa270Udc {
    struct PxaDma *dma;
    struct PxaIc *ic;

    struct Ep ep[24];
    uint32_t udccr, udcicr[2], udcisr[2], udcotgicr, udcotgisr, up2ocr;
    uint16_t udcfnr;
    uint8_t up3ocr;

    // CPU-side endpoint 0 storage. Bus events come from the host entry points.
    uint8_t rx0[16], tx0[16];
    uint8_t rx0Pos, tx0Count;
    // EP0 control transfer in progress, for UDA: 0 none, 1 control read (status is a
    // zero-length OUT), 2 control write or no-data (status is a zero-length IN).
    uint8_t ctl0;

    EpFifo fifo[24];         // [0] unused
    uint32_t halted;         // bit n: endpoint n STALLs until CLEAR_FEATURE(ENDPOINT_HALT)
    uint8_t address, pendingAddress;
    uint8_t alt[8];          // alternate setting per interface number
    uint8_t memSwitched;     // SMAC done since the last SET_CONFIGURATION/SET_INTERFACE
    uint8_t hw0, hw0Flags, hw0Len;
    uint8_t hw0Data[2];
    // Host side only, not saved: UDE 0->1 count, whether UDCICR was written since
    // the latest enable, and who to tell about guest register writes.
    uint32_t enableCount;
    bool icrSinceEnable;
    void (*notify)(void *);
    void *notifyContext;

    template <typename T>
    void DoSaveLoad(T &h, uint32_t version) {
        for (auto &e : ep) h.Do32(e.ccra).Do16(e.csr).Do16(e.bcr);
        h.Do32(udccr).DoBuffer32(udcicr, 2).DoBuffer32(udcisr, 2)
            .Do32(udcotgicr).Do32(udcotgisr).Do32(up2ocr).Do16(udcfnr).Do8(up3ocr);
        if (version >= 1)
            h.DoBuffer(rx0, sizeof(rx0)).DoBuffer(tx0, sizeof(tx0))
                .Do8(rx0Pos).Do8(tx0Count);
        if (version >= 2) h.Do8(ctl0);
        if (version >= 3) {
            for (auto &f : fifo)
                h.DoBuffer(f.data, sizeof(f.data)).DoBuffer(f.size, sizeof(f.size))
                    .Do8(f.head).Do8(f.count).Do8(f.fill).Do8(f.pos);
            h.Do32(halted).Do8(address).Do8(pendingAddress).DoBuffer(alt, sizeof(alt))
                .Do8(memSwitched).Do8(hw0).Do8(hw0Flags).Do8(hw0Len).DoBuffer(hw0Data, sizeof(hw0Data));
        }
    }
};

// Intel PXA27x Developer's Manual 280000-003, sections 12.4.1,
// 12.4.2 and 12.6.5-12.6.11. ISR latches are independent of endpoint status.
static void pxa270UdcPrvIrq(Pxa270Udc *udc) {
    pxaIcInt(udc->ic, PXA_I_USB, (udc->udcisr[0] & udc->udcicr[0]) ||
             (udc->udcisr[1] & udc->udcicr[1]) || (udc->udcotgisr & udc->udcotgicr));
}

static void pxa270UdcPrvFlushTx0(Pxa270Udc *udc) {
    memset(udc->tx0, 0, sizeof(udc->tx0));
    udc->tx0Count = 0;
    udc->ep[0].csr &= ~0x02; // IPR; flushing is not successful transmission
}

static void pxa270UdcPrvFlushRx0(Pxa270Udc *udc) {
    memset(udc->rx0, 0, sizeof(udc->rx0));
    udc->rx0Pos = 0;
    udc->ep[0].bcr = 0;
    udc->ep[0].csr &= ~0x40; // RNE; OPC is acknowledged separately
}

// UDCCRx fields (manual 12.6.11).
static bool pxa270UdcPrvEpIn(uint32_t c) { return c & 0x1000; }
static unsigned pxa270UdcPrvEpMps(uint32_t c) { return (c >> 2) & 0x3ff; }
static unsigned pxa270UdcPrvEpType(uint32_t c) { return (c >> 13) & 3; } // 1 iso, 2 bulk, 3 interrupt
static unsigned pxa270UdcPrvEpBuffers(uint32_t c) { return (c & 2) ? 2 : 1; }
static unsigned pxa270UdcPrvEpCapacity(uint32_t c) {
    return std::max(1u, std::min(64u, pxa270UdcPrvEpMps(c)));
}

static void pxa270UdcPrvFlushEp(Pxa270Udc *udc, unsigned n) {
    memset(&udc->fifo[n], 0, sizeof(udc->fifo[n]));
}

static void pxa270UdcPrvEpEvent(Pxa270Udc *udc, unsigned n, unsigned type) {
    // Two bits per endpoint, packet complete (0) and FIFO error (1); Q-X are in ISR1.
    const unsigned word = n < 16 ? 0 : 1, bit = 2 * (n < 16 ? n : n - 16) + type;
    udc->udcisr[word] |= udc->udcicr[word] & (1ul << bit); // ICR gates new events only
}

// Endpoint memory check when UDE is set (manual 12.4.2, 12.6.1.3): maximum packet sizes
// valid for the type and at most 4064 bytes per configuration/interface/alternate setting.
// Configuration, interface and endpoint numbers are not checked, as on the silicon.
static bool pxa270UdcPrvConfigValid(const Pxa270Udc *udc) {
    unsigned used[256] = {};
    for (unsigned n = 1; n < 24; ++n) {
        const uint32_t c = udc->ep[n].ccra;
        if (!(c & 1)) continue;
        const unsigned mps = pxa270UdcPrvEpMps(c), type = pxa270UdcPrvEpType(c);
        if ((type == 1 && !mps) || (type == 2 && mps != 8 && mps != 16 && mps != 32 && mps != 64) ||
            (type == 3 && (!mps || mps > 64)))
            return false;
        auto &group = used[(c >> 19) & 255];
        group += ((mps + 3) & ~3u) * pxa270UdcPrvEpBuffers(c);
        if (group > 4064) return false;
    }
    return true;
}

// Endpoint A-X that belongs to the configuration and alternate settings the host chose.
static bool pxa270UdcPrvEpActive(const Pxa270Udc *udc, unsigned n) {
    const uint32_t c = udc->ep[n].ccra;
    const unsigned acn = (udc->udccr >> 11) & 3;
    return (c & 1) && acn && ((c >> 25) & 3) == acn && udc->alt[(c >> 22) & 7] == ((c >> 19) & 7);
}

static unsigned pxa270UdcPrvFindEp(const Pxa270Udc *udc, uint8_t endpoint, bool in) {
    for (unsigned n = 1; n < 24; ++n)
        if (pxa270UdcPrvEpActive(udc, n) && ((udc->ep[n].ccra >> 15) & 15) == (endpoint & 15u) &&
            pxa270UdcPrvEpIn(udc->ep[n].ccra) == in)
            return n;
    return 0;
}

// Bus reset and UDE=0 drop everything the host set up and every A-X FIFO.
static void pxa270UdcPrvResetDevice(Pxa270Udc *udc) {
    for (unsigned n = 1; n < 24; ++n) pxa270UdcPrvFlushEp(udc, n);
    udc->halted = 0;
    udc->address = udc->pendingAddress = 0;
    memset(udc->alt, 0, sizeof(udc->alt));
    udc->memSwitched = 0;
    udc->hw0 = udc->hw0Flags = udc->hw0Len = 0;
    memset(udc->hw0Data, 0, sizeof(udc->hw0Data));
}

static uint16_t pxa270UdcPrvReadCsrX(const Pxa270Udc *udc, unsigned n) {
    const uint32_t c = udc->ep[n].ccra;
    const EpFifo &f = udc->fifo[n];
    uint16_t v = udc->ep[n].csr & CSRX_STORED;
    if (pxa270UdcPrvEpIn(c)) {
        // Single buffer: room until one complete packet is loaded. Double: until two.
        if (f.count < pxa270UdcPrvEpBuffers(c)) v |= CSRX_FS | CSRX_BNE;
        for (unsigned i = 0; i < f.count; ++i)
            if (f.size[(f.head + i) & 1] < pxa270UdcPrvEpCapacity(c)) v |= CSRX_SP;
    } else if (f.count) {
        v |= CSRX_FS;
        if (f.pos < f.size[f.head]) v |= CSRX_BNE;
        if (f.size[f.head] < pxa270UdcPrvEpMps(c)) v |= CSRX_SP;
    }
    return v;
}

static void pxa270UdcPrvWriteCsrX(Pxa270Udc *udc, unsigned n, uint32_t val) {
    auto &e = udc->ep[n];
    auto &f = udc->fifo[n];
    const bool in = pxa270UdcPrvEpIn(e.ccra);
    const bool stalled = e.csr & CSRX_SST;
    e.csr = ((e.csr & ~(CSRX_DME | CSRX_FST)) | (val & (CSRX_DME | CSRX_FST))) &
            ~(val & (CSRX_SST | CSRX_TRN | CSRX_PC)) & CSRX_STORED;
    // FEF flushes; for IN endpoints, clearing a set SST also flushes the transmit FIFO.
    if ((val & CSRX_FEF) || (in && stalled && (val & CSRX_SST))) pxa270UdcPrvFlushEp(udc, n);
    if (in) {
        // SP commits the bytes loaded so far (none: a zero-length packet).
        if ((val & CSRX_SP) && f.count < pxa270UdcPrvEpBuffers(e.ccra)) {
            f.size[(f.head + f.count) & 1] = f.fill;
            f.count++;
            f.fill = 0;
        }
    } else if ((val & CSRX_PC) && f.count) {
        // Clearing PC drops whatever is left of the packet and shows the next one.
        f.head = (f.head + 1) & 1;
        f.count--;
        f.pos = 0;
        if (f.count) e.csr |= CSRX_PC;
    }
}

static bool pxa270UdcPrvFifoX(Pxa270Udc *udc, unsigned n, uint_fast8_t size, bool write, void *buf) {
    const uint32_t c = udc->ep[n].ccra;
    // With the UDC or the endpoint disabled, reads return 0 and writes are ignored.
    if (!(udc->udccr & UDCCR_UDE) || !(c & 1)) {
        if (!write) memset(buf, 0, 4);
        return true;
    }
    auto &f = udc->fifo[n];
    if (write) {
        // Writes to an OUT endpoint, to a full IN FIFO or past the packet size are dropped.
        if (!pxa270UdcPrvEpIn(c) || f.count >= pxa270UdcPrvEpBuffers(c)) return true;
        uint32_t value = 0;
        if (size == 1) value = *static_cast<uint8_t *>(buf);
        else if (size == 2) { uint16_t v; memcpy(&v, buf, 2); value = v; }
        else memcpy(&value, buf, 4);
        const unsigned capacity = pxa270UdcPrvEpCapacity(c), index = (f.head + f.count) & 1;
        for (unsigned i = 0; i < size && f.fill < capacity; ++i) f.data[index][f.fill++] = value >> (8 * i);
        if (f.fill == capacity) {
            f.size[index] = f.fill;
            f.count++;
            f.fill = 0;
        }
    } else {
        uint32_t value = 0;
        if (!pxa270UdcPrvEpIn(c) && f.count) {
            const unsigned left = f.size[f.head] - f.pos, count = std::min(4u, left);
            for (unsigned i = 0; i < count; ++i) value |= uint32_t(f.data[f.head][f.pos++]) << (8 * i);
        }
        memcpy(buf, &value, 4);
    }
    return true;
}

static bool pxa270UdcPrvFifo0(Pxa270Udc *udc, uint_fast8_t size, bool write, void *buf) {
    if (!(udc->udccr & 1)) return false;
    if (write) {
        // A packet committed with IPR is not changed by further writes; they are
        // dropped without a guest fault. The manual leaves this case undefined.
        if (udc->ep[0].csr & 2) return true;
        // Narrow writes are only for the final partial word (caller contract).
        // Reject an overflowing access atomically; do not invent a USB FIFO-error
        // interrupt for an invalid processor access.
        if (udc->tx0Count + size > sizeof(udc->tx0)) return false;
        uint32_t value = 0;
        if (size == 1) value = *static_cast<uint8_t *>(buf);
        else if (size == 2) { uint16_t v; memcpy(&v, buf, 2); value = v; }
        else memcpy(&value, buf, 4);
        for (unsigned i = 0; i < size; ++i) udc->tx0[udc->tx0Count++] = value >> (8 * i);
        if (udc->tx0Count == sizeof(udc->tx0)) udc->ep[0].csr |= 2;
    } else {
        uint32_t value = 0;
        const unsigned count = std::min<unsigned>(4, udc->ep[0].bcr);
        for (unsigned i = 0; i < count; ++i) value |= uint32_t(udc->rx0[udc->rx0Pos++]) << (8 * i);
        udc->ep[0].bcr -= count;
        if (!udc->ep[0].bcr) udc->ep[0].csr &= ~0x40;
        // Empty/unused lanes read as zero; never return stale packet data.
        memcpy(buf, &value, 4);
    }
    return true;
}

static void pxa270UdcPrvNotify(Pxa270Udc *udc) {
    if (udc->notify) udc->notify(udc->notifyContext);
}

static bool pxa270UdcPrvMemAccessF(void *userData, uint32_t pa, uint_fast8_t size, bool write,
                                   void *buf) {
    struct Pxa270Udc *udc = (struct Pxa270Udc *)userData;
    uint32_t val = 0;

    if (pa < PXA_UDC_BASE || pa >= PXA_UDC_BASE + PXA_UDC_SIZE || (pa & 3)) return false;
    const bool fifo = pa >= PXA_UDC_BASE + 0x300 && pa < PXA_UDC_BASE + 0x360;
    if (size != 4 && !(fifo && write && (size == 1 || size == 2))) return false;
    if (write) pxa270UdcPrvNotify(udc);
    if (pa == PXA_UDC_BASE + 0x300) return pxa270UdcPrvFifo0(udc, size, write, buf);
    if (fifo) return pxa270UdcPrvFifoX(udc, (pa - PXA_UDC_BASE - 0x300) >> 2, size, write, buf);

    pa = (pa - PXA_UDC_BASE) >> 2;
    if (write) memcpy(&val, buf, 4);

    if (pa >= 0x100 / 4 && pa < 0x160 / 4) {
        uint_fast8_t epNo = pa - 0x100 / 4;

        if (!write)
            val = epNo ? pxa270UdcPrvReadCsrX(udc, epNo) : udc->ep[0].csr;
        else if (!epNo) {
            // ACM/DME are R/W; AREN/FST/IPR are W1S; SA/SST/OPC are W1C.
            // FTF (reads zero) drops bytes still being loaded. A packet already
            // committed with IPR stays until the host ACKs it or a SETUP/OUT, bus
            // reset or UDE=0 drops it: the TX ROM's XLLP driver writes FTF after
            // every EP0 interrupt, right after loading the next packet. Judged on
            // CSR0 before this write, so FTF|IPR flushes first and sends a ZLP.
            auto &csr = udc->ep[0].csr;
            if ((val & 0x004) && !(csr & 0x002)) pxa270UdcPrvFlushTx0(udc);
            csr = ((csr & ~0x208) | (val & 0x208) | (val & 0x122)) & ~(val & 0x091);
            if (val & 0x001) pxa270UdcPrvFlushRx0(udc);
        } else if (udc->udccr & UDCCR_UDE) {
            pxa270UdcPrvWriteCsrX(udc, epNo, val);
        }
    } else if (pa >= 0x200 / 4 && pa < 0x260 / 4) {
        uint_fast8_t epNo = pa - 0x200 / 4;

        if (write)
            ;  // ignored
        else if (!epNo)
            val = udc->ep[0].bcr;
        else {
            const auto &f = udc->fifo[epNo];
            val = (!pxa270UdcPrvEpIn(udc->ep[epNo].ccra) && f.count) ? f.size[f.head] - f.pos : 0;
        }
    } else if (pa >= 0x404 / 4 && pa < 0x460 / 4) {
        uint_fast8_t epNo = pa - 0x400 / 4;

        if (write && !(udc->udccr & 1)) {
            udc->ep[epNo].ccra = val & 0x07fffffful;
        } else
            val = udc->ep[epNo].ccra;
    } else
        switch (pa) {
            case 0x000 / 4:  // UDCCR
                if (write) {
                    const bool disabling = (udc->udccr & 1) && !(val & 1);
                    bool enabling = !(udc->udccr & 1) && (val & 1);
                    udc->udccr = ((udc->udccr & 0x70011ffeul) | (val & 0x80000001ul)) &
                                 ~(val & UDCCR_EMCE); // EMCE is W1C
                    // Manual 12.6.1.3: UDE checks the endpoint memory map; an invalid one
                    // sets EMCE and leaves the UDC off. EMCE must be cleared first.
                    if (enabling && ((udc->udccr & UDCCR_EMCE) || !pxa270UdcPrvConfigValid(udc))) {
                        udc->udccr = (udc->udccr & ~UDCCR_UDE) | UDCCR_EMCE;
                        enabling = false;
                    }
                    if (enabling) {
                        udc->enableCount++;
                        udc->icrSinceEnable = false;
                    }
                    // SMAC switches the endpoint memory to the active configuration: the
                    // A-X FIFOs are flushed and their bulk traffic stops NAKing.
                    if ((val & UDCCR_SMAC) && (udc->udccr & UDCCR_UDE)) {
                        for (unsigned n = 1; n < 24; ++n) {
                            pxa270UdcPrvFlushEp(udc, n);
                            udc->ep[n].csr &= ~(CSRX_PC | CSRX_TRN);
                        }
                        udc->memSwitched = 1;
                    }
                    // UDR is ignored without host-enabled remote wakeup. SMAC self-clears
                    // as soon as the memory is switched.
                    udc->udccr &= ~0x14ul;
                    if (disabling) {
                        pxa270UdcPrvFlushTx0(udc);
                        pxa270UdcPrvFlushRx0(udc);
                        for (auto &e : udc->ep) { e.csr = 0; e.bcr = 0; }
                        udc->udccr &= 0x80000009ul;
                        udc->ctl0 = 0;
                        pxa270UdcPrvResetDevice(udc);
                    }
                } else val = udc->udccr;
                break;

            case 0x04 / 4:  // UDICICR0
                if (write) {
                    udc->udcicr[0] = val;
                    if (udc->udccr & UDCCR_UDE) udc->icrSinceEnable = true;
                } else
                    val = udc->udcicr[0];
                break;

            case 0x08 / 4:  // UDICICR1
                if (write) {
                    udc->udcicr[1] = val & 0xf800fffful;
                    if (udc->udccr & UDCCR_UDE) udc->icrSinceEnable = true;
                } else
                    val = udc->udcicr[1];
                break;

            case 0x0c / 4:  // UDICISR0
                if (write)
                    udc->udcisr[0] &= ~val;
                else
                    val = udc->udcisr[0];
                break;

            case 0x10 / 4:  // UDICISR1
                if (write)
                    udc->udcisr[1] &= ~(val & 0xf800fffful);
                else
                    val = udc->udcisr[1];
                break;

            case 0x14 / 4:  // UDCFNR
                if (write)
                    ;  // ignored
                else
                    val = udc->udcfnr;
                break;

            case 0x18 / 4:  // UDCOTGICR
                if (write)
                    udc->udcotgicr = val & 0x010303fful;
                else
                    val = udc->udcotgicr;
                break;

            case 0x1c / 4:  // UDCOTGISR
                if (write)
                    udc->udcotgisr &= ~(val & 0x010303fful);
                else
                    val = udc->udcotgisr;
                break;

            case 0x20 / 4:  // UP2OCR
                if (write)
                    udc->up2ocr = val & 0x070307fful;
                else
                    val = udc->up2ocr;
                break;

            case 0x24 / 4:  // UP3OCR
                if (write)
                    udc->up3ocr = val & 3;
                else
                    val = udc->up3ocr;
                break;

            default:
                return false;
        }

    if (!write) memcpy(buf, &val, 4);
    else {
        pxa270UdcPrvIrq(udc);
        pxa270UdcPrvNotify(udc);
    }

    return true;
}

static void pxa270UdcPrvPacketComplete0(Pxa270Udc *udc) {
    udc->udcisr[0] |= udc->udcicr[0] & 1; // ICR gates new events only
    pxa270UdcPrvIrq(udc);
}

static void pxa270UdcPrvEndControl0(Pxa270Udc *udc) {
    udc->ctl0 = 0;
    udc->hw0 = HW0_NONE;
    udc->hw0Flags = 0;
    udc->udccr &= ~2ul; // UDA
}

bool pxa270UdcHostReset(Pxa270Udc *udc) {
    if (!(udc->udccr & 1)) return false;
    pxa270UdcPrvFlushTx0(udc);
    pxa270UdcPrvFlushRx0(udc);
    for (auto &e : udc->ep) { e.csr = 0; e.bcr = 0; }
    // Manual 12.6.1: a USB reset resets UDCCR (UDE and OEN stay, the UDC is still
    // enabled), so UDA and ACN/AIN/AAISN clear. Latched ISR bits are left for the
    // guest's W1C; reset only adds IRRS.
    udc->udccr &= 0x80000001ul;
    udc->ctl0 = 0;
    pxa270UdcPrvResetDevice(udc);
    udc->udcisr[1] |= udc->udcicr[1] & (1u << 27); // IRRS
    pxa270UdcPrvIrq(udc);
    return true;
}

bool pxa270UdcHostFrame(Pxa270Udc *udc) {
    if (!(udc->udccr & 1)) return false;
    udc->udcfnr = (udc->udcfnr + 1) & 0x7ff;
    udc->udcisr[1] |= udc->udcicr[1] & (1u << 30); // IRSOF
    pxa270UdcPrvIrq(udc);
    return true;
}

uint8_t pxa270UdcHostAddress(const Pxa270Udc *udc) { return udc->address; }

bool pxa270UdcHostSofWanted(const Pxa270Udc *udc) { return udc->udcicr[1] & (1u << 30); }

bool pxa270UdcHostEnabled(const Pxa270Udc *udc) { return udc->udccr & UDCCR_UDE; }

bool pxa270UdcHostListening(const Pxa270Udc *udc) {
    return (udc->udccr & UDCCR_UDE) && udc->icrSinceEnable && (udc->udcicr[0] & 1) &&
           (udc->udcicr[1] & (1u << 27));
}

void pxa270UdcHostSetNotify(Pxa270Udc *udc, void (*notify)(void *), void *context) {
    udc->notify = notify;
    udc->notifyContext = context;
}

uint32_t pxa270UdcHostEnableCount(const Pxa270Udc *udc) { return udc->enableCount; }

// Standard requests the UDC decodes itself (manual 12.4.4). Returns false for requests
// it forwards to the guest through the EP0 FIFO.
static bool pxa270UdcPrvHardwareRequest(Pxa270Udc *udc, const uint8_t setup[8]) {
    if (setup[0] & 0x60) return false; // class and vendor requests
    const unsigned request = setup[1], recipient = setup[0] & 0x1f;
    const unsigned value = setup[2] | setup[3] << 8, index = setup[4] | setup[5] << 8;
    const unsigned length = setup[6] | setup[7] << 8;
    const unsigned acn = (udc->udccr >> 11) & 3;
    auto stall = [&] { udc->hw0 = HW0_STALL; };
    auto reply = [&](uint8_t a, uint8_t b, unsigned size) {
        udc->hw0Data[0] = a;
        udc->hw0Data[1] = b;
        udc->hw0Len = std::min(size, length);
        udc->hw0 = length ? HW0_DATA : HW0_STATUS_IN;
    };
    auto configured = [&](unsigned endpoint) {
        return pxa270UdcPrvFindEp(udc, endpoint & 15, endpoint & 0x80);
    };
    auto changed = [&] {
        // IRCC (if IECC is enabled). Bulk/interrupt traffic NAKs until the guest sets SMAC.
        udc->memSwitched = 0;
        udc->udcisr[1] |= udc->udcicr[1] & 0x80000000ul;
        udc->hw0 = HW0_STATUS_IN;
        udc->hw0Flags |= HW0_GATED;
    };
    switch (request) {
        case 0:  // GET_STATUS
            if (recipient == 0) reply((udc->udccr & UDCCR_DRWE) ? 2 : 0, 0, 2);
            else if (recipient == 1) reply(0, 0, 2);
            else if (recipient == 2 && !(index & 0x0f)) reply(0, 0, 2);
            else if (recipient == 2 && configured(index)) reply((udc->halted >> configured(index)) & 1, 0, 2);
            else stall();
            break;
        case 1:  // CLEAR_FEATURE
        case 3:  // SET_FEATURE
            udc->hw0 = HW0_STATUS_IN;
            if (recipient == 0 && value == 1) {  // DEVICE_REMOTE_WAKEUP
                udc->udccr = request == 3 ? (udc->udccr | UDCCR_DRWE) : (udc->udccr & ~UDCCR_DRWE);
            } else if (recipient == 0 && request == 3 && value >= 3 && value <= 5) {
                // OTG b_hnp_enable/a_hnp_support/a_alt_hnp_support: STALL unless OEN.
                if (udc->udccr & 0x80000000ul) udc->udccr |= 1ul << (25 + value);
                else stall();
            } else if (recipient == 2 && value == 0 && (index & 0x0f)) {  // ENDPOINT_HALT
                if (const unsigned n = configured(index)) {
                    if (request == 3) udc->halted |= 1ul << n;
                    else { udc->halted &= ~(1ul << n); udc->ep[n].csr &= ~CSRX_FST; }
                } else stall();
            } else if (!(recipient == 2 && value == 0) && !(recipient == 0 && value == 2) && recipient != 1) {
                stall();
            }
            break;
        case 5:  // SET_ADDRESS, applied once the status stage completes
            udc->pendingAddress = value & 0x7f;
            udc->hw0 = HW0_STATUS_IN;
            udc->hw0Flags |= HW0_ADDRESS;
            break;
        case 8:  // GET_CONFIGURATION
            reply(acn, 0, 1);
            break;
        case 9:  // SET_CONFIGURATION
            if (value > 3) { stall(); break; }
            udc->udccr = (udc->udccr & ~UDCCR_ACTIVE) | (uint32_t(value) << 11);
            memset(udc->alt, 0, sizeof(udc->alt));
            udc->halted = 0;
            changed();
            break;
        case 10:  // GET_INTERFACE
            if (!acn || index > 7) stall();
            else reply(udc->alt[index], 0, 1);
            break;
        case 11:  // SET_INTERFACE
            if (!acn || index > 7 || value > 7) { stall(); break; }
            udc->alt[index] = value;
            // Endpoints of that interface start un-halted (USB 2.0 9.4.10).
            for (unsigned n = 1; n < 24; ++n) {
                const uint32_t c = udc->ep[n].ccra;
                if ((c & 1) && ((c >> 25) & 3) == acn && ((c >> 22) & 7) == index) {
                    udc->halted &= ~(1ul << n);
                    udc->ep[n].csr &= ~(CSRX_FST | CSRX_SST);
                }
            }
            udc->udccr = (udc->udccr & ~0x7e0ul) | (uint32_t(index) << 8) | (uint32_t(value) << 5);
            changed();
            break;
        default:  // GET_DESCRIPTOR, SET_DESCRIPTOR, SYNCH_FRAME and unknown requests
            return false;
    }
    return true;
}

Pxa270UdcHostResult pxa270UdcHostSetup(Pxa270Udc *udc, const uint8_t setup[8]) {
    if (!(udc->udccr & 1)) return Pxa270UdcHostResult::Disabled;
    if (udc->ep[0].csr & 8) return Pxa270UdcHostResult::Unsupported; // EP0 OUT DMA
    // SETUP is always accepted. It is an OUT-direction packet, so it drops a ready IN
    // packet without IR0, and it ends a pending STALL and AREN.
    pxa270UdcPrvFlushTx0(udc);
    udc->hw0 = HW0_NONE;
    udc->hw0Flags = 0;
    // UDA from SETUP until the status stage is ACKed or the transfer is STALLed.
    udc->ctl0 = (setup[0] & 0x80) ? 1 : 2;
    udc->udccr |= 2;
    if (pxa270UdcPrvHardwareRequest(udc, setup)) {
        // Answered by the UDC: nothing reaches the EP0 FIFO and IR0 stays quiet.
        udc->ep[0].csr &= ~0x120;
        udc->ctl0 = udc->hw0 == HW0_DATA ? 1 : 2; // a no-data request ends with a status IN
        pxa270UdcPrvIrq(udc);
        return Pxa270UdcHostResult::Ack;
    }
    pxa270UdcPrvFlushRx0(udc);
    memcpy(udc->rx0, setup, 8);
    udc->ep[0].bcr = 8;
    udc->ep[0].csr = (udc->ep[0].csr & 0x218) | 0xc1; // keep ACM/SST/DME; SA, RNE, OPC
    pxa270UdcPrvPacketComplete0(udc);
    return Pxa270UdcHostResult::Ack;
}

Pxa270UdcHostResult pxa270UdcHostOut(Pxa270Udc *udc, const uint8_t *data, unsigned size) {
    if (!(udc->udccr & 1)) return Pxa270UdcHostResult::Disabled;
    if (size > sizeof(udc->rx0) || (size && !data) || (udc->ep[0].csr & 8))
        return Pxa270UdcHostResult::Unsupported;
    if (udc->hw0) {
        // Status OUT of a request the UDC answered; anything else is a protocol error.
        const bool status = !size && (udc->hw0 == HW0_DATA || udc->hw0 == HW0_STATUS_OUT);
        pxa270UdcPrvEndControl0(udc);
        return status ? Pxa270UdcHostResult::Ack : Pxa270UdcHostResult::Stall;
    }
    auto &csr = udc->ep[0].csr;
    if (csr & 0x20) { pxa270UdcPrvEndControl0(udc); csr &= ~0x20; return Pxa270UdcHostResult::Stall; } // FST self-clears
    if (csr & 1) return Pxa270UdcHostResult::Nak; // RX packet not yet released
    // Manual 12.4.1: the EP0 TX FIFO is flushed when an OUT data packet arrives.
    pxa270UdcPrvFlushTx0(udc);
    pxa270UdcPrvFlushRx0(udc);
    if (size) memcpy(udc->rx0, data, size);
    udc->ep[0].bcr = size;
    csr |= 1 | (size ? 0x40 : 0);
    if (!size && udc->ctl0 == 1) pxa270UdcPrvEndControl0(udc); // status of a control read
    pxa270UdcPrvPacketComplete0(udc);
    return Pxa270UdcHostResult::Ack;
}

Pxa270UdcHostResult pxa270UdcHostIn(Pxa270Udc *udc, uint8_t data[16], unsigned *size) {
    *size = 0;
    if (!(udc->udccr & 1)) return Pxa270UdcHostResult::Disabled;
    auto &csr = udc->ep[0].csr;
    switch (udc->hw0) {
        case HW0_DATA:
            *size = udc->hw0Len;
            memcpy(data, udc->hw0Data, udc->hw0Len);
            udc->hw0 = HW0_STATUS_OUT;
            return Pxa270UdcHostResult::Ack;
        case HW0_STATUS_IN:
            // SET_CONFIGURATION/SET_INTERFACE wait for AREN while ACM is set.
            if ((udc->hw0Flags & HW0_GATED) && (csr & 0x200) && !(csr & 0x100)) return Pxa270UdcHostResult::Nak;
            if (udc->hw0Flags & HW0_ADDRESS) udc->address = udc->pendingAddress;
            pxa270UdcPrvEndControl0(udc);
            return Pxa270UdcHostResult::Ack;
        case HW0_STALL:
            pxa270UdcPrvEndControl0(udc);
            return Pxa270UdcHostResult::Stall;
        case HW0_STATUS_OUT:
            return Pxa270UdcHostResult::Nak;
    }
    if (csr & 0x20) { pxa270UdcPrvEndControl0(udc); csr &= ~0x20; return Pxa270UdcHostResult::Stall; } // FST self-clears
    // No data stage until the guest releases the SETUP/OUT packet, and NAK until IPR.
    // An IN with IPR clear is only NAKed; no FIFO-underrun event is raised.
    if ((csr & 0x81) || !(csr & 2)) return Pxa270UdcHostResult::Nak;
    *size = udc->tx0Count;
    memcpy(data, udc->tx0, udc->tx0Count);
    pxa270UdcPrvFlushTx0(udc); // sent and ACKed
    if (!*size && udc->ctl0 == 2) pxa270UdcPrvEndControl0(udc); // status of a control write
    pxa270UdcPrvPacketComplete0(udc);
    return Pxa270UdcHostResult::Ack;
}

// Common checks for a bulk/interrupt token. Returns the A-X index in *n on Ack.
static Pxa270UdcHostResult pxa270UdcPrvEpToken(const Pxa270Udc *udc, uint8_t endpoint, bool in, unsigned *n) {
    *n = 0;
    if (!(udc->udccr & 1)) return Pxa270UdcHostResult::Disabled;
    if (!(endpoint & 15) || (endpoint & ~0x8fu)) return Pxa270UdcHostResult::Unsupported;
    *n = pxa270UdcPrvFindEp(udc, endpoint, in);
    if (!*n) return Pxa270UdcHostResult::Timeout;
    const auto &e = udc->ep[*n];
    // Isochronous endpoints and endpoint DMA are not modelled.
    if (pxa270UdcPrvEpType(e.ccra) == 1 || (e.csr & CSRX_DME)) return Pxa270UdcHostResult::Unsupported;
    if ((e.csr & CSRX_FST) || (udc->halted & (1ul << *n))) return Pxa270UdcHostResult::Stall;
    // Manual 12.6.1.2: bulk and interrupt tokens NAK from SET_CONFIGURATION until SMAC.
    if (!udc->memSwitched) return Pxa270UdcHostResult::Nak;
    return Pxa270UdcHostResult::Ack;
}

// FST: the STALL handshake goes out, FST clears, SST sets and the endpoint stays halted.
static Pxa270UdcHostResult pxa270UdcPrvEpStall(Pxa270Udc *udc, unsigned n) {
    auto &e = udc->ep[n];
    if (e.csr & CSRX_FST) {
        e.csr = (e.csr & ~CSRX_FST) | CSRX_SST;
        udc->halted |= 1ul << n;
    }
    return Pxa270UdcHostResult::Stall;
}

Pxa270UdcHostResult pxa270UdcHostEpIn(Pxa270Udc *udc, uint8_t endpoint, uint8_t data[64], unsigned *size) {
    *size = 0;
    unsigned n;
    const auto result = pxa270UdcPrvEpToken(udc, endpoint, true, &n);
    if (result == Pxa270UdcHostResult::Stall) return pxa270UdcPrvEpStall(udc, n);
    if (result != Pxa270UdcHostResult::Ack) return result;
    auto &e = udc->ep[n];
    auto &f = udc->fifo[n];
    if (!f.count) { e.csr |= CSRX_TRN; return Pxa270UdcHostResult::Nak; }
    *size = f.size[f.head];
    memcpy(data, f.data[f.head], *size);
    memset(f.data[f.head], 0, sizeof(f.data[f.head]));
    f.size[f.head] = 0;
    f.head = (f.head + 1) & 1;
    f.count--;
    e.csr |= CSRX_PC;
    pxa270UdcPrvEpEvent(udc, n, 0);
    pxa270UdcPrvIrq(udc);
    return Pxa270UdcHostResult::Ack;
}

Pxa270UdcHostResult pxa270UdcHostEpOut(Pxa270Udc *udc, uint8_t endpoint, const uint8_t *data, unsigned size) {
    unsigned n;
    const auto result = pxa270UdcPrvEpToken(udc, endpoint, false, &n);
    if (result == Pxa270UdcHostResult::Ack || result == Pxa270UdcHostResult::Nak ||
        result == Pxa270UdcHostResult::Stall) {
        // Manual 12.4.2: a bulk/interrupt packet over the maximum size gets no handshake.
        if (size > pxa270UdcPrvEpMps(udc->ep[n].ccra) || size > 64) return Pxa270UdcHostResult::Timeout;
        if (size && !data) return Pxa270UdcHostResult::Unsupported;
    }
    if (result == Pxa270UdcHostResult::Stall) return pxa270UdcPrvEpStall(udc, n);
    if (result != Pxa270UdcHostResult::Ack) return result;
    auto &e = udc->ep[n];
    auto &f = udc->fifo[n];
    if (f.count >= pxa270UdcPrvEpBuffers(e.ccra)) { e.csr |= CSRX_TRN; return Pxa270UdcHostResult::Nak; }
    const unsigned index = (f.head + f.count) & 1;
    memset(f.data[index], 0, sizeof(f.data[index]));
    if (size) memcpy(f.data[index], data, size);
    f.size[index] = size;
    if (!f.count) f.pos = 0;
    f.count++;
    e.csr |= CSRX_PC;
    pxa270UdcPrvEpEvent(udc, n, 0);
    pxa270UdcPrvIrq(udc);
    return Pxa270UdcHostResult::Ack;
}

Pxa270UdcHostResult pxa270UdcHostPeekIn(const Pxa270Udc *udc, uint8_t endpoint, unsigned *size) {
    *size = 0;
    if (!(udc->udccr & 1)) return Pxa270UdcHostResult::Disabled;
    if (!(endpoint & 15)) {
        const auto csr = udc->ep[0].csr;
        switch (udc->hw0) {
            case HW0_DATA: *size = udc->hw0Len; return Pxa270UdcHostResult::Ack;
            case HW0_STATUS_IN:
                return ((udc->hw0Flags & HW0_GATED) && (csr & 0x200) && !(csr & 0x100))
                           ? Pxa270UdcHostResult::Nak : Pxa270UdcHostResult::Ack;
            case HW0_STALL: return Pxa270UdcHostResult::Stall;
            case HW0_STATUS_OUT: return Pxa270UdcHostResult::Nak;
        }
        if (csr & 0x20) return Pxa270UdcHostResult::Stall;
        if ((csr & 0x81) || !(csr & 2)) return Pxa270UdcHostResult::Nak;
        *size = udc->tx0Count;
        return Pxa270UdcHostResult::Ack;
    }
    unsigned n;
    const auto result = pxa270UdcPrvEpToken(udc, endpoint, true, &n);
    if (result != Pxa270UdcHostResult::Ack) return result;
    const auto &f = udc->fifo[n];
    if (!f.count) return Pxa270UdcHostResult::Nak;
    *size = f.size[f.head];
    return Pxa270UdcHostResult::Ack;
}

struct Pxa270Udc *pxa270UdcInit(struct ArmMem *physMem, struct PxaIc *ic, struct PxaDma *dma) {
    struct Pxa270Udc *udc = (struct Pxa270Udc *)malloc(sizeof(*udc));

    if (!udc) ERR("cannot alloc UDC");

    memset(udc, 0, sizeof(*udc));

    udc->ic = ic;
    udc->dma = dma;
    udc->up2ocr = 0x00030000ul;

    if (!memRegionAdd(physMem, PXA_UDC_BASE, PXA_UDC_SIZE, pxa270UdcPrvMemAccessF, udc))
        ERR("cannot add UDC to MEM\n");

    pxa270UdcPrvIrq(udc);
    return udc;
}


template <typename T>
void pxa270UdcSave(Pxa270Udc *state, T &savestate) {
    auto chunk = savestate.GetChunk(ChunkType::pxa270udc, 3);
    if (!chunk) ERR("cannot allocate pxa270udc state");
    SaveChunkHelper helper(*chunk);
    state->DoSaveLoad(helper, 3);
}

void pxa270UdcLoad(Pxa270Udc *state, SavestateLoader<ChunkType> &loader) {
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::pxa270udc, 3, "pxa270udc", version);
    if (!chunk) return;
    // Validate before publishing any loaded state or driving the interrupt line.
    Pxa270Udc restored = *state;
    LoadChunkHelper helper(*chunk);
    restored.DoSaveLoad(helper, version);
    // v1 had no transfer direction; its only UDA source was a control read.
    if (version < 2) restored.ctl0 = (restored.udccr & 2) ? 1 : 0;
    if (version < 3) {
        // No A-X FIFOs, address or host configuration before v3. Their CSRs keep only
        // stored bits; FS/BNE/SP now come from the (empty) FIFOs.
        pxa270UdcPrvResetDevice(&restored);
        for (unsigned i = 1; i < 24; ++i) { restored.ep[i].csr &= CSRX_STORED; restored.ep[i].bcr = 0; }
        restored.memSwitched = (restored.udccr >> 11) & 3 ? 1 : 0;
    }
    if (version == 0) {
        // The old stub had no FIFOs and treated ISR acknowledgements as writes.
        // Its saved ISR/packet bits must not turn into phantom traffic/IRQs.
        pxa270UdcPrvFlushTx0(&restored);
        pxa270UdcPrvFlushRx0(&restored);
        restored.ep[0].csr &= 0x328;
        for (unsigned i = 1; i < 24; ++i) { restored.ep[i].csr &= 0x028; restored.ep[i].bcr = 0; }
        restored.udcisr[0] = restored.udcisr[1] = restored.udcotgisr = 0;
        restored.udccr &= 0x80000001ul;
        restored.ctl0 = 0;
        restored.memSwitched = 0;
    }
    bool valid = !chunk->HasError() && restored.rx0Pos <= 16 && restored.tx0Count <= 16 &&
                 restored.ctl0 <= 2 && !restored.ctl0 == !(restored.udccr & 2) &&
                 restored.ep[0].bcr <= 16 - restored.rx0Pos && restored.hw0 <= HW0_STATUS_OUT &&
                 restored.hw0Len <= 2 && restored.hw0Flags <= 3 && (!restored.hw0 || restored.ctl0) &&
                 restored.address < 128 && restored.pendingAddress < 128 && restored.memSwitched <= 1;
    for (unsigned i = 0; i < 8; ++i) valid = valid && restored.alt[i] < 8;
    for (unsigned i = 1; i < 24; ++i) {
        const auto &f = restored.fifo[i];
        valid = valid && f.head < 2 && f.count <= 2 && f.fill <= 64 && f.pos <= 64 && f.size[0] <= 64 &&
                f.size[1] <= 64 && (!f.count || f.pos <= f.size[f.head]) && !(restored.ep[i].csr & ~CSRX_STORED);
    }
    if (!valid) {
        loader.NotifyError();
        return;
    }
    // Only documented bits participate in IRQ generation, including on restore.
    restored.udcicr[1] &= 0xf800fffful;
    restored.udcisr[1] &= 0xf800fffful;
    restored.udcotgicr &= 0x010303fful;
    restored.udcotgisr &= 0x010303fful;
    // A session saved with the UDC on and its interrupts set up is taken as listening.
    restored.icrSinceEnable = restored.udccr & UDCCR_UDE;
    *state = restored;
    pxa270UdcPrvIrq(state);
}

template void pxa270UdcSave<Savestate<ChunkType>>(Pxa270Udc *, Savestate<ChunkType> &);
template void pxa270UdcSave<SavestateProbe<ChunkType>>(Pxa270Udc *, SavestateProbe<ChunkType> &);
