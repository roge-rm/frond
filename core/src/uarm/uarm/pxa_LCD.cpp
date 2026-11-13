//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_LCD.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "CPEndian.h"
#include "SoC.h"
#include "cputil.h"
#include "mem.h"
#include "mem_internal.h"
#include "memory_buffer.h"
#include "pxa_IC.h"
#include "savestate/savestateAll.h"

#define PXA_LCD_BASE 0x44000000UL
#define PXA_LCD_SIZE 0x00001000UL

#define LCD_STATE_IDLE 0
#define LCD_STATE_DMA_0_START 1
#define LCD_STATE_DMA_0_END 2

#define UNMASKABLE_INTS 0x7C8E

#define PALETTE_SIZE 512
#define SAVESTATE_VERSION 2

struct PxaLcd {
    struct PxaIc *ic;
    struct ArmMem *mem;
    class SoC *soc;

    // registers
    uint32_t lccr0, lccr1, lccr2, lccr3, lccr4, lccr5, liicr, trgbr, tcr;
    uint32_t fbr[7], fdadr[7], fsadr[7], fidr[7], ldcmd[7];
    uint16_t lcsr;  // yes, 16-bit :)
    // PXA27x overlays 1 and 2, and LCSR1 with their per-channel SOF, EOF and branch bits.
    uint32_t ovl1c1, ovl1c2, ovl2c1, ovl2c2, ccr, lcsr1;
    bool pxa270;  // overlays, channels 2-6 and LCSR1 exist only on the PXA27x

    // for our use
    uint16_t intMask;

    uint8_t state;
    uint8_t intWasPending;
    uint8_t enbChanged;

    MemoryBuffer paletteBuffer;
    uint32_t palette_mapped[512];

    uint16_t width;
    uint16_t height;

    uint32_t *front_buffer;
    uint32_t *back_buffer;

    uint32_t i_pixel;
    bool frame_pending;

    uint32_t frameNum;

    uint32_t framebufferBase;
    uint32_t framebufferSize;
    uint8_t framebufferBpp;
    bool framebufferDirty;
    bool framebufferTrackingActive;

    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version) {
        chunkHelper.Do32(lccr0)
            .Do32(lccr1)
            .Do32(lccr2)
            .Do32(lccr3)
            .Do32(lccr4)
            .Do32(lccr5)
            .Do32(liicr)
            .Do32(trgbr)
            .Do32(tcr)
            .Do(typename T::Pack16() << lcsr << intMask)
            .Do(typename T::Pack8() << state << intWasPending << enbChanged << framebufferBpp)
            .Do32(i_pixel)
            .Do32(framebufferBase)
            .Do32(framebufferSize)
            .DoBool(framebufferTrackingActive);

        for (size_t i = 0; i < (version >= 1 ? 7u : 2u); i++)
            chunkHelper.Do32(fbr[i]).Do32(fdadr[i]).Do32(fsadr[i]).Do32(fidr[i]).Do32(ldcmd[i]);
        if (version >= 2)
            chunkHelper.Do32(ovl1c1).Do32(ovl1c2).Do32(ovl2c1).Do32(ovl2c2).Do32(ccr).Do32(lcsr1);
    }
};

static inline uint32_t unpack_rgb16(uint16_t rgb16) {
    uint8_t r = (rgb16 >> 11) & 0x1f;
    uint8_t g = (rgb16 >> 5) & 0x3f;
    uint8_t b = (rgb16 >> 0) & 0x1f;

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);

    return 0xff000000 | (b << 16) | (g << 8) | r;
}

static void pxaLcdPrvUpdateInts(struct PxaLcd *lcd) {
    // LCSR1's bits interrupt unless LCCR5 masks them.
    uint32_t ints = (lcd->lcsr & lcd->intMask) | (lcd->lcsr1 & ~lcd->lccr5 & 0x3e3f3f3fUL);

    if ((ints && !lcd->intWasPending) || (!ints && lcd->intWasPending)) {
        lcd->intWasPending = !!ints;
        pxaIcInt(lcd->ic, PXA_I_LCD, !!ints);
    }
}

static void pxaLcdPrvFrameSizeChanged(struct PxaLcd *lcd, uint32_t oldLccr1, uint32_t oldLccr2);

static bool pxaLcdPrvMemAccessF(void *userData, uint32_t pa, uint_fast8_t size, bool write,
                                void *buf) {
    struct PxaLcd *lcd = (struct PxaLcd *)userData;
    uint_fast16_t v16;
    uint32_t val = 0;

    if (size != 4) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;  // we do not support non-word accesses
    }

    pa = (pa - PXA_LCD_BASE) >> 2;

    if (write) {
        val = *(uint32_t *)buf;

        switch (pa) {
            case 0:
                if ((lcd->lccr0 ^ val) & 0x0001)  // something changed about enablement - handle it
                    lcd->enbChanged = 1;

                lcd->lccr0 = val;
                // recalc intMask
                v16 = UNMASKABLE_INTS;
                if (val & 0x00200000UL)  // output fifo underrun
                    v16 |= 0x0040;
                if (val & 0x00100000UL)  // branch int
                    v16 |= 0x0200;
                if (val & 0x00000800UL)  // quick disable
                    v16 |= 0x0001;
                if (val & 0x00000040UL)  // end of frame
                    v16 |= 0x0080;
                if (val & 0x00000020UL)  // input fifo underrun
                    v16 |= 0x0030;
                if (val & 0x00000010UL)  // start of frame
                    v16 |= 0x0002;
                lcd->intMask = v16;
                pxaLcdPrvUpdateInts(lcd);
                break;

            case 1:
            case 2: {
                const uint32_t oldLccr1 = lcd->lccr1, oldLccr2 = lcd->lccr2;
                if (pa == 1)
                    lcd->lccr1 = val;
                else
                    lcd->lccr2 = val;
                pxaLcdPrvFrameSizeChanged(lcd, oldLccr1, oldLccr2);
                break;
            }

            case 3:
                lcd->lccr3 = val;
                break;

            case 4:
                lcd->lccr4 = val;
                break;

            case 5:
                lcd->lccr5 = val;
                break;

            case 68:
            case 69:
                pa -= 69;
                pa += 13;
                pa += 1;
                // fallthrough
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:  // FBR4
                lcd->fbr[pa - 8] = val;
                break;

            case 13:  // LCSR1, write 1 to clear
                lcd->lcsr1 &= ~val;
                pxaLcdPrvUpdateInts(lcd);
                break;

            case 20:
                lcd->ovl1c1 = val;
                lcd->framebufferDirty = true;
                break;

            case 24:
                lcd->ovl1c2 = val;
                lcd->framebufferDirty = true;
                break;

            case 28:
                // Turning overlay 2 on in planar 4:2:0 underruns its input FIFO (LCSR1 IU2);
                // Palm OS waits for that, up to 1 s, before reloading FDADR2-4 (0x2009bce4).
                // The FIFO only underruns while the controller is running.
                if (lcd->pxa270 && (lcd->lccr0 & 1) && (val & ~lcd->ovl2c1 & 0x80000000UL) &&
                    ((lcd->ovl2c2 >> 20) & 7) == 4) {
                    lcd->lcsr1 |= 1UL << 25;
                    pxaLcdPrvUpdateInts(lcd);
                }
                lcd->ovl2c1 = val;
                lcd->framebufferDirty = true;
                break;

            case 32:
                lcd->ovl2c2 = val;
                lcd->framebufferDirty = true;
                break;

            case 36:
                lcd->ccr = val;
                break;

            case 14:
                lcd->lcsr &= ~val;
                pxaLcdPrvUpdateInts(lcd);
                break;

            case 15:
                lcd->liicr = val;
                break;

            case 16:
                lcd->trgbr = val;
                break;

            case 17:
                lcd->tcr = val;
                break;

            case 128:
            case 132:
            case 136:
            case 140:
            case 144:
            case 148:
            case 152:
                lcd->fdadr[(pa - 128) / 4] = val;
                break;
        }
    } else {
        switch (pa) {
            case 0:
                val = lcd->lccr0;
                break;

            case 1:
                val = lcd->lccr1;
                break;

            case 2:
                val = lcd->lccr2;
                break;

            case 3:
                val = lcd->lccr3;
                break;

            case 4:
                val = lcd->lccr4;
                break;

            case 5:
                val = lcd->lccr5;
                break;

            case 68:
            case 69:
                pa -= 69;
                pa += 13;
                pa += 1;
                // fallthrough
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
                val = lcd->fbr[pa - 8];
                break;

            case 13:
                val = lcd->lcsr1;
                break;

            case 20:
                val = lcd->ovl1c1;
                break;

            case 24:
                val = lcd->ovl1c2;
                break;

            case 28:
                val = lcd->ovl2c1;
                break;

            case 32:
                val = lcd->ovl2c2;
                break;

            case 36:
                val = lcd->ccr;
                break;

            case 14:
                val = lcd->lcsr;
                break;

            case 15:
                val = lcd->liicr;
                break;

            case 16:
                val = lcd->trgbr;
                break;

            case 17:
                val = lcd->tcr;
                break;

            case 128:
            case 132:
            case 136:
            case 140:
            case 144:
            case 148:
            case 152:
                val = lcd->fdadr[(pa - 128) / 4];
                break;

            case 129:
            case 133:
            case 137:
            case 141:
            case 145:
            case 149:
            case 153:
                val = lcd->fsadr[(pa - 129) / 4];
                break;

            case 130:
            case 134:
            case 138:
            case 142:
            case 146:
            case 150:
            case 154:
                val = lcd->fidr[(pa - 130) / 4];
                break;

            case 131:
            case 135:
            case 139:
            case 143:
            case 147:
            case 151:
            case 155:
                val = lcd->ldcmd[(pa - 131) / 4];
                break;
        }
        *(uint32_t *)buf = val;
    }

    return true;
}

static uint32_t pxaLcdPrvGetWord(struct PxaLcd *lcd, uint32_t addr) {
    uint32_t v;

    if (!memAccess<4, false>(lcd->mem, addr, &v)) return 0;

    return v;
}

static void pxaLcdPrvDma(struct PxaLcd *lcd, void *dest, uint32_t addr, int32_t len) {
    uint8_t *d = (uint8_t *)dest;
    uint32_t t;

    // we assume aligntment here both on part of dest and of addr

    while (len > 0) {
        t = pxaLcdPrvGetWord(lcd, addr);
        if (len-- > 0) *d++ = t;
        if (len-- > 0) *d++ = t >> 8;
        if (len-- > 0) *d++ = t >> 16;
        if (len-- > 0) *d++ = t >> 24;
        addr += 4;
    }
}

static void pxaLcdUpdatePalette(struct PxaLcd *lcd, int32_t len) {
    const uint32_t n_entries = len >> 1;
    uint16_t *entry = (uint16_t *)lcd->paletteBuffer.buffer;
    uint32_t *entry_mapped = (uint32_t *)lcd->palette_mapped;

    bool dirty = false;

    for (uint32_t i = 0; i < n_entries; i++, entry++, entry_mapped++) {
        uint32_t unpacked = unpack_rgb16(htole16(*entry));
        dirty = dirty || (unpacked != *entry_mapped);

        if (dirty) *entry_mapped = unpack_rgb16(*entry);
    }

    if (dirty) lcd->soc->SetFramebufferDirty();
}

static void pxaLcdPrvScreenDataPixel(struct PxaLcd *lcd, uint32_t color) {
    lcd->back_buffer[lcd->i_pixel++] = color;

    if (lcd->i_pixel == lcd->width * lcd->height) {
        lcd->i_pixel = 0;
        lcd->frame_pending = true;

        uint32_t *front_buffer = lcd->front_buffer;
        lcd->front_buffer = lcd->back_buffer;
        lcd->back_buffer = front_buffer;
    }
}

// 16 bpp frame data read straight from primary RAM: the same pixels, flips and i_pixel as
// the word-by-word loop below. False leaves anything else (other regions, a held RAM,
// unaligned data) to that loop.
static bool pxaLcdPrvScreenData16FromRam(struct PxaLcd *lcd, uint32_t addr, uint32_t len) {
    const uint32_t words = len / 4;
    const uint32_t total = uint32_t(lcd->width) * lcd->height;
    const struct ArmMemRegion &region = lcd->mem->regions[REGION_RAM];
    if (!region.ub || (addr & 3) || addr < region.pa ||
        uint64_t(addr) + uint64_t(words) * 4 > region.ub || lcd->i_pixel >= total)
        return false;

    const struct ArmRam *ram = (const struct ArmRam *)region.uD;
    const uint8_t *src = (const uint8_t *)ram->buf.buffer + (addr - ram->adr);
    uint32_t pixels = words * 2;

    while (pixels) {
        const uint32_t run = std::min(pixels, total - lcd->i_pixel);
        uint32_t *dst = lcd->back_buffer + lcd->i_pixel;

        for (uint32_t i = 0; i < run; i++) {
            uint16_t rgb16;
            memcpy(&rgb16, src + i * 2, sizeof(rgb16));
            dst[i] = unpack_rgb16(rgb16);
        }

        src += run * 2;
        pixels -= run;
        lcd->i_pixel += run;

        if (lcd->i_pixel == total) {
            lcd->i_pixel = 0;
            lcd->frame_pending = true;

            uint32_t *front_buffer = lcd->front_buffer;
            lcd->front_buffer = lcd->back_buffer;
            lcd->back_buffer = front_buffer;
        }
    }

    return true;
}

static bool pxaLcdPrvOverlaysOn(const struct PxaLcd *lcd) {
    return (lcd->ovl1c1 | lcd->ovl2c1) & 0x80000000UL;
}

// Takes the next descriptor of DMA channel ch, or the branch in FBRch, as the base plane's
// channel 0 does once a frame, and sets that channel's SOF, EOF and branch bits in LCSR1.
static void pxaLcdPrvOverlayDescriptor(struct PxaLcd *lcd, unsigned ch) {
    uint32_t descrAddr;
    if (lcd->fbr[ch] & 1) {
        lcd->fbr[ch] &= ~1UL;
        if (lcd->fbr[ch] & 2) lcd->lcsr1 |= 1UL << (16 + ch - 1);
        descrAddr = lcd->fbr[ch] & ~0xFUL;
    } else {
        descrAddr = lcd->fdadr[ch];
    }
    lcd->fdadr[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 0);
    lcd->fsadr[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 4);
    lcd->fidr[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 8);
    lcd->ldcmd[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 12);
    if (lcd->ldcmd[ch] & 0x00400000UL) lcd->lcsr1 |= 1UL << (ch - 1);
    if (lcd->ldcmd[ch] & 0x00200000UL) lcd->lcsr1 |= 1UL << (8 + ch - 1);
}

// Overlay 2's YCbCr format: 0 RGB, 1 packed, 2 to 4 planar on channels 2 (Y), 3 (Cb) and 4 (Cr).
static inline uint32_t pxaLcdPrvOverlay2Format(const struct PxaLcd *lcd) { return (lcd->ovl2c2 >> 20) & 7; }

// Each enabled overlay channel takes its next descriptor once a base frame, as on the PXA27x,
// whether or not Frond can draw the overlay's format.
static void pxaLcdPrvOverlayFrame(struct PxaLcd *lcd) {
    if (lcd->ovl1c1 & 0x80000000UL) pxaLcdPrvOverlayDescriptor(lcd, 1);
    if (lcd->ovl2c1 & 0x80000000UL) {
        pxaLcdPrvOverlayDescriptor(lcd, 2);
        const uint32_t format = pxaLcdPrvOverlay2Format(lcd);
        if (format >= 2 && format <= 4) {
            pxaLcdPrvOverlayDescriptor(lcd, 3);
            pxaLcdPrvOverlayDescriptor(lcd, 4);
        }
    }
    // An overlay is turned off by clearing OEN, loading FBRx with a branch and waiting for
    // LCSR1 BSx, so a stopped channel still takes a branch (the Zire 72 Camera waits up to 1 s
    // for it at 0x2009bdac). It loads the branch's descriptor as a running channel does, so
    // FDADRx reads back that descriptor's next address, but moves no data and sets only BSx.
    if (!lcd->pxa270) return;
    for (unsigned ch = 1; ch <= 4; ch++) {
        const uint32_t c1 = ch == 1 ? lcd->ovl1c1 : lcd->ovl2c1;
        if ((c1 & 0x80000000UL) || !(lcd->fbr[ch] & 1)) continue;
        lcd->fbr[ch] &= ~1UL;
        if (lcd->fbr[ch] & 2) lcd->lcsr1 |= 1UL << (16 + ch - 1);
        const uint32_t descrAddr = lcd->fbr[ch] & ~0xFUL;
        lcd->fdadr[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 0);
        lcd->fsadr[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 4);
        lcd->fidr[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 8);
        lcd->ldcmd[ch] = pxaLcdPrvGetWord(lcd, descrAddr + 12);
    }
}

// Reads one plane of `lines` rows from channel ch's current descriptor. The row stride is the
// DMA length over the rows, so padded rows work; false if the data is shorter than
// `bytesPerLine` a row.
static bool pxaLcdPrvOverlayPlane(struct PxaLcd *lcd, unsigned ch, uint32_t bytesPerLine, uint32_t lines,
                                  std::vector<uint8_t> &plane, uint32_t &stride) {
    const uint32_t len = lcd->ldcmd[ch] & 0x001FFFFFUL;
    if (!lines || len / lines < bytesPerLine) return false;
    stride = len / lines;
    plane.resize((size_t)stride * lines);
    pxaLcdPrvDma(lcd, plane.data(), lcd->fsadr[ch], stride * lines);
    return true;
}

static inline uint8_t pxaLcdPrvClamp8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// CCIR 601-2 studio range as in the PXA27x manual 7.4.9.2 (Equation 7-1), saturated to 0-255.
// The constants are the coefficients (K1-K5 / 256) times 65536. Full-range input, like the
// Zire 72 camera's, comes out with more contrast, as on the hardware.
static inline uint32_t pxaLcdPrvYCbCr(int32_t y, int32_t cb, int32_t cr) {
    y = (y - 16) * 76309;
    cb -= 128;
    cr -= 128;
    const uint8_t r = pxaLcdPrvClamp8((y + 104597 * cr + 32768) >> 16);
    const uint8_t g = pxaLcdPrvClamp8((y - 25675 * cb - 53279 * cr + 32768) >> 16);
    const uint8_t b = pxaLcdPrvClamp8((y + 132201 * cb + 32768) >> 16);
    return 0xff000000 | (b << 16) | (g << 8) | r;
}

// Draws one overlay over the finished frame. c1 and c2 are its OVLxC1 and OVLxC2. Overlay 1
// is RGB on channel 1; overlay 2 is RGB or packed YCbCr on channel 2, or planar YCbCr on
// channels 2 (Y), 3 (Cb) and 4 (Cr). Only 16 bpp RGB is drawn.
static void pxaLcdPrvDrawOverlay(struct PxaLcd *lcd, uint32_t *frame, unsigned overlay, uint32_t c1,
                                 uint32_t c2) {
    const uint32_t w = (c1 & 0x3FF) + 1, h = ((c1 >> 10) & 0x3FF) + 1;
    const uint32_t x0 = c2 & 0x3FF, y0 = (c2 >> 10) & 0x3FF;
    const uint32_t format = overlay == 2 ? pxaLcdPrvOverlay2Format(lcd) : 0;
    const uint32_t bpp = (c1 >> 20) & 0xF;
    const unsigned ch = overlay == 1 ? 1 : 2;
    std::vector<uint8_t> y, cb, cr;
    uint32_t ys = 0, cbs = 0, crs = 0;
    // Chroma plane size by format: 2 planar 4:4:4, 3 planar 4:2:2, 4 planar 4:2:0.
    const uint32_t cw = format == 2 ? w : (w + 1) / 2, chh = format == 4 ? (h + 1) / 2 : h;

    if (format == 0) {
        if (bpp != 4 || !pxaLcdPrvOverlayPlane(lcd, ch, w * 2, h, y, ys)) return;
    } else if (format == 1) {
        if (!pxaLcdPrvOverlayPlane(lcd, 2, w * 4, h, y, ys)) return;
    } else if (format >= 2 && format <= 4) {
        if (!pxaLcdPrvOverlayPlane(lcd, 2, w, h, y, ys) || !pxaLcdPrvOverlayPlane(lcd, 3, cw, chh, cb, cbs) ||
            !pxaLcdPrvOverlayPlane(lcd, 4, cw, chh, cr, crs))
            return;
    } else {
        return;
    }

    for (uint32_t row = 0; row < h && y0 + row < lcd->height; row++) {
        uint32_t *dst = frame + (y0 + row) * lcd->width;
        for (uint32_t col = 0; col < w && x0 + col < lcd->width; col++) {
            if (format == 0) {
                uint16_t rgb16;
                memcpy(&rgb16, &y[row * ys + col * 2], 2);
                dst[x0 + col] = unpack_rgb16(le16toh(rgb16));
            } else if (format == 1) {
                // Packed 4:4:4 words hold Y in bits 7:0, Cb in 15:8 and Cr in 23:16.
                const uint8_t *p = &y[row * ys + col * 4];
                dst[x0 + col] = pxaLcdPrvYCbCr(p[0], p[1], p[2]);
            } else {
                const uint32_t cx = format == 2 ? col : col / 2, cy = format == 4 ? row / 2 : row;
                dst[x0 + col] = pxaLcdPrvYCbCr(y[row * ys + col], cb[cy * cbs + cx], cr[cy * crs + cx]);
            }
        }
    }
}

static void pxaLcdPrvDrawOverlays(struct PxaLcd *lcd, uint32_t *frame) {
    if (lcd->ovl1c1 & 0x80000000UL) pxaLcdPrvDrawOverlay(lcd, frame, 1, lcd->ovl1c1, lcd->ovl1c2);
    if (lcd->ovl2c1 & 0x80000000UL) pxaLcdPrvDrawOverlay(lcd, frame, 2, lcd->ovl2c1, lcd->ovl2c2);
}

// A frame programmed in LCCR1 (PPL) and LCCR2 (LPP) larger than the panel, with a DMA length
// to match. The panel shows the centre of it: Palm Linux on the T|X sets 324x484 with the
// horizontal and vertical back porches 2 shorter than Palm OS's 320x480, so the panel's first
// pixel is the frame's (2, 2). False for anything else, such as a frame the panel's size.
static bool pxaLcdPrvLargerFrame(const struct PxaLcd *lcd, uint32_t lccr1, uint32_t lccr2,
                                 uint8_t bpp, uint32_t len, uint32_t &ppl, uint32_t &lpp) {
    ppl = (lccr1 & 0x3ff) + 1;
    lpp = (lccr2 & 0x3ff) + 1;
    if (bpp > 4 || ppl < lcd->width || lpp < lcd->height) return false;
    if (ppl == lcd->width && lpp == lcd->height) return false;
    return len == ((ppl * lpp) << bpp) >> 3;
}

// Draws the panel's part of a larger frame (see above) and finishes it.
static void pxaLcdPrvScreenDataLarger(struct PxaLcd *lcd, uint32_t addr, uint32_t len, uint8_t bpp,
                                      uint32_t ppl, uint32_t lpp) {
    std::vector<uint8_t> frame(len);
    pxaLcdPrvDma(lcd, frame.data(), addr, len);

    const uint32_t x0 = (ppl - lcd->width) / 2, y0 = (lpp - lcd->height) / 2;
    const uint32_t bits = 1u << bpp;
    for (uint32_t y = 0; y < lcd->height; y++) {
        uint32_t *dst = lcd->back_buffer + y * lcd->width;
        for (uint32_t x = 0; x < lcd->width; x++) {
            const uint32_t bit = ((y0 + y) * ppl + x0 + x) * bits;
            const uint8_t *src = &frame[bit >> 3];
            if (bpp == 4)
                dst[x] = unpack_rgb16(src[0] | (src[1] << 8));
            else
                dst[x] = lcd->palette_mapped[(src[0] >> (bit & 7)) & ((1u << bits) - 1)];
        }
    }

    lcd->i_pixel = 0;
    lcd->frame_pending = true;
    std::swap(lcd->front_buffer, lcd->back_buffer);
}

// Decides whether the DMA'd framebuffer is tracked by the SoC, and marks it dirty.
static void pxaLcdPrvTrackFramebuffer(struct PxaLcd *lcd, bool larger) {
    const uint32_t panelBytes = ((uint32_t)(lcd->width * lcd->height) << lcd->framebufferBpp) >> 3;
    if (larger || lcd->framebufferSize == panelBytes) {
        lcd->framebufferDirty = true;
        lcd->framebufferTrackingActive =
            lcd->soc->SetFramebuffer(lcd->framebufferBase, lcd->framebufferSize);
    } else {
        lcd->soc->SetFramebuffer(0, 0);
        lcd->framebufferDirty = true;
        lcd->framebufferTrackingActive = false;
    }
}

// A change of LCCR1/LCCR2 that takes the frame in or out of the larger kind redoes the tracking
// decision for the DMA seen so far, so a shrunk frame isn't left stale and a larger frame seen
// before LCCR was programmed gets tracked.
static void pxaLcdPrvFrameSizeChanged(struct PxaLcd *lcd, uint32_t oldLccr1, uint32_t oldLccr2) {
    uint32_t ppl, lpp;
    if (!lcd->framebufferSize) return;
    const bool was = pxaLcdPrvLargerFrame(lcd, oldLccr1, oldLccr2, lcd->framebufferBpp,
                                          lcd->framebufferSize, ppl, lpp);
    const bool is = pxaLcdPrvLargerFrame(lcd, lcd->lccr1, lcd->lccr2, lcd->framebufferBpp,
                                         lcd->framebufferSize, ppl, lpp);
    if (was != is) pxaLcdPrvTrackFramebuffer(lcd, is);
}

static void pxaLcdPrvScreenDataDma(struct PxaLcd *lcd, uint32_t addr /*PA*/, uint32_t len) {
    uint8_t data[4];
    uint32_t i, j;
    const uint8_t bpp = (lcd->lccr3 >> 24) & 7;
    const uint32_t *const frameBefore = lcd->front_buffer;
    uint32_t ppl, lpp;
    const bool larger = pxaLcdPrvLargerFrame(lcd, lcd->lccr1, lcd->lccr2, bpp, len, ppl, lpp);

    if (addr != lcd->framebufferBase || len != lcd->framebufferSize || bpp != lcd->framebufferBpp) {
        lcd->framebufferBase = addr;
        lcd->framebufferSize = len;
        lcd->framebufferBpp = bpp;

        pxaLcdPrvTrackFramebuffer(lcd, larger);
    }

    // An overlay's planes aren't tracked, so a frame with one is always drawn.
    const bool overlays = pxaLcdPrvOverlaysOn(lcd);
    if (lcd->framebufferTrackingActive && !lcd->framebufferDirty && !overlays) return;

    if (larger) {
        pxaLcdPrvScreenDataLarger(lcd, addr, len, bpp, ppl, lpp);
        len = 0;
    } else if (bpp == 4 && pxaLcdPrvScreenData16FromRam(lcd, addr, len)) {
        len = 0;
    }

    len /= 4;
    while (len--) {
        pxaLcdPrvDma(lcd, data, addr, 4);
        addr += 4;

        switch (bpp) {
            case 0:  // 1BPP

                for (i = 0; i < 4; i++) {
                    for (j = 0; j < 8; j++)
                        pxaLcdPrvScreenDataPixel(lcd, lcd->palette_mapped[((data[i] >> j) & 1)]);
                }
                break;

            case 1:  // 2BPP

                for (i = 0; i < 4; i++) {
                    for (j = 0; j < 8; j += 2)
                        pxaLcdPrvScreenDataPixel(lcd, lcd->palette_mapped[((data[i] >> j) & 3)]);
                }
                break;

            case 2:  // 4BPP

                for (i = 0; i < 4; i++) {
                    for (j = 0; j < 8; j += 4)
                        pxaLcdPrvScreenDataPixel(lcd, lcd->palette_mapped[((data[i] >> j) & 15)]);
                }
                break;

            case 3:  // 8BPP

                for (i = 0; i < 4; i++) pxaLcdPrvScreenDataPixel(lcd, lcd->palette_mapped[data[i]]);

                break;

            case 4:  // 16BPP

                pxaLcdPrvScreenDataPixel(lcd, unpack_rgb16(*(uint16_t *)(&data[0])));
                pxaLcdPrvScreenDataPixel(lcd, unpack_rgb16(*(uint16_t *)(&data[2])));
                break;

            default:

                ;  // BAD
        }
    }

    // The base plane finished a frame: the overlays go on top of it.
    if (overlays && lcd->front_buffer != frameBefore) pxaLcdPrvDrawOverlays(lcd, lcd->front_buffer);

    lcd->framebufferDirty = false;
    lcd->soc->ClearFramebufferDirty();
}

void pxaLcdTick(struct PxaLcd *lcd) {
    // every other call starts a frame, the others end one [this generates spacing between
    // interrupts so as to not confuse guest OS]

    if (lcd->enbChanged) {
        if (lcd->lccr0 & 0x0001) {  // just got enabled
            lcd->framebufferDirty = true;
            // TODO: perhaps check settings?
        } else {  // we just got quick disabled - kill current frame and do no more

            lcd->lcsr |= 0x0080;  // quick disable happened
            lcd->state = LCD_STATE_IDLE;
        }
        lcd->enbChanged = false;
    }

    if (lcd->lccr0 & 0x0001) {  // enabled - do a frame

        uint32_t descrAddr, len;

        if (lcd->lccr0 & 0x400) {  // got disabled

            lcd->lcsr |= 0x0001;  // disable happened
            lcd->state = LCD_STATE_IDLE;
            lcd->lccr0 &= ~1;
        } else
            switch (lcd->state) {
                case LCD_STATE_IDLE:

                    if (lcd->fbr[0] & 1) {  // branch

                        lcd->fbr[0] &= ~1UL;
                        if (lcd->fbr[0] & 2) lcd->lcsr |= 0x0200;
                        descrAddr = lcd->fbr[0] & ~0xFUL;
                    } else
                        descrAddr = lcd->fdadr[0];
                    lcd->fdadr[0] = pxaLcdPrvGetWord(lcd, descrAddr + 0);
                    lcd->fsadr[0] = pxaLcdPrvGetWord(lcd, descrAddr + 4);
                    lcd->fidr[0] = pxaLcdPrvGetWord(lcd, descrAddr + 8);
                    lcd->ldcmd[0] = pxaLcdPrvGetWord(lcd, descrAddr + 12);

                    lcd->state = LCD_STATE_DMA_0_START;
                    break;

                case LCD_STATE_DMA_0_START:

                    if (lcd->ldcmd[0] & 0x00400000UL)
                        lcd->lcsr |= 0x0002;  // set SOF is DMA 0 started
                    len = lcd->ldcmd[0] & 0x000FFFFFUL;

                    if (lcd->ldcmd[0] & 0x04000000UL) {  // pallette data

                        if (len > PALETTE_SIZE) len = PALETTE_SIZE;

                        pxaLcdPrvDma(lcd, lcd->paletteBuffer.buffer, lcd->fsadr[0], len);
                        memoryBufferMarkRangeDirty(&lcd->paletteBuffer, 0, len);

                        pxaLcdUpdatePalette(lcd, len);
                    } else {
                        lcd->frameNum++;
                        pxaLcdPrvOverlayFrame(lcd);

                        if (!(lcd->frameNum & 63)) pxaLcdPrvScreenDataDma(lcd, lcd->fsadr[0], len);
                    }

                    lcd->state = LCD_STATE_DMA_0_END;
                    break;

                case LCD_STATE_DMA_0_END:

                    if (lcd->ldcmd[0] & 0x00200000UL)
                        lcd->lcsr |= 0x0100;  // set EOF is DMA 0 finished
                    lcd->state = LCD_STATE_IDLE;
                    break;
            }
    }
    pxaLcdPrvUpdateInts(lcd);
}

uint32_t *pxaLcdGetPendingFrame(struct PxaLcd *lcd) {
    return lcd->frame_pending ? lcd->front_buffer : NULL;
}

void pxaLcdResetPendingFrame(struct PxaLcd *lcd) { lcd->frame_pending = false; }

void pxaLcdSetFramebufferDirty(struct PxaLcd *lcd) { lcd->framebufferDirty = true; }

bool pxaLcdIsEnabled(struct PxaLcd *lcd) { return lcd->lccr0 & 0x0001; }

struct PxaLcd *pxaLcdInit(struct ArmMem *physMem, class SoC *soc, struct PxaIc *ic,
                          struct MemoryBuffer *buffer, uint16_t width, uint16_t height, bool pxa270) {
    struct PxaLcd *lcd = (struct PxaLcd *)malloc(sizeof(*lcd));

    if (!lcd) ERR("cannot alloc LCD");

    memset(lcd, 0, sizeof(*lcd));
    lcd->ic = ic;
    lcd->mem = physMem;
    lcd->intMask = UNMASKABLE_INTS;
    lcd->width = width;
    lcd->height = height;
    lcd->soc = soc;
    lcd->pxa270 = pxa270;

    lcd->front_buffer = (uint32_t *)malloc(width * height * 4);
    lcd->back_buffer = (uint32_t *)malloc(width * height * 4);

    memcpy(&lcd->paletteBuffer, buffer, sizeof(*buffer));

    if (!memRegionAdd(physMem, PXA_LCD_BASE, PXA_LCD_SIZE, pxaLcdPrvMemAccessF, lcd))
        ERR("cannot add LCD to MEM\n");

    return lcd;
}

void pxaLcdResetPaletteBuffer(struct PxaLcd *lcd) {
    memset(lcd->paletteBuffer.buffer, 0, PALETTE_SIZE);
    memoryBufferMarkRangeDirty(&lcd->paletteBuffer, 0, PALETTE_SIZE);
}

template <typename T>
void pxaLcdSave(struct PxaLcd *lcd, T &savestate, bool pxa270) {
    const uint32_t version = pxa270 ? SAVESTATE_VERSION : 0;
    auto chunk = savestate.GetChunk(ChunkType::pxaLcd, version);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    lcd->DoSaveLoad(helper, version);
}

template <typename T>
void pxaLcdLoad(struct PxaLcd *lcd, T &loader) {
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaLcd, SAVESTATE_VERSION, "pxaLcd", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    lcd->ovl1c1 = lcd->ovl1c2 = lcd->ovl2c1 = lcd->ovl2c2 = lcd->ccr = lcd->lcsr1 = 0;
    lcd->DoSaveLoad(helper, version);

    // Host frame buffers are rebuilt from guest RAM after loading.
    lcd->framebufferDirty = true;
    lcd->frame_pending = false;
    // The frame counter isn't saved; it picks which frames are drawn, so a load starts it
    // from 0 as a fresh process does, whatever ran before.
    lcd->frameNum = 0;
    memset(lcd->front_buffer, 0, lcd->width * lcd->height * 4);
    memset(lcd->back_buffer, 0, lcd->width * lcd->height * 4);

    pxaLcdUpdatePalette(lcd, PALETTE_SIZE);

    if (lcd->framebufferTrackingActive) {
        lcd->framebufferTrackingActive =
            lcd->soc->SetFramebuffer(lcd->framebufferBase, lcd->framebufferSize);
    } else {
        lcd->soc->SetFramebuffer(0, 0);
    }
}

template void pxaLcdSave<Savestate<ChunkType>>(PxaLcd *lcd, Savestate<ChunkType> &savestate, bool);
template void pxaLcdSave<SavestateProbe<ChunkType>>(PxaLcd *lcd,
                                                    SavestateProbe<ChunkType> &savestate, bool);
template void pxaLcdLoad<SavestateLoader<ChunkType>>(PxaLcd *lcd,
                                                     SavestateLoader<ChunkType> &loader);
