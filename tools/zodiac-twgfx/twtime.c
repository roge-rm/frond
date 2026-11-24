/* Zodiac TwGfxWriteSurface timing program: writes a full-screen 320x480 RGB565 buffer (300 KB, the size
 * Firehammer writes every frame) and a small 64x64 one to accelerator surfaces, each call timed with
 * TwGetMicroSeconds, and shows the mean, min and max in ms. A tap runs it again, any button quits.
 *
 * The results also go on the screen as a row of 16x16 blocks at the display's bottom, 40 pixels from the
 * right, each block's RGB565 colour one 16-bit word: f81f, 07e0, the phase (2 running, 3 done, 5 no memory),
 * full mean, min and max in us, small mean and min in us, f81f. tools/zodiac-twgfx/twtime.py reads them from a
 * benchmark-replay snapshot. */
#include "twgfx.h"

#define slotFntSetFont 112
#define slotKeyCurrentState 125
#define slotMemPtrFree 136
#define slotWinSetTextColor 298
#define slotTwGetMicroSeconds 464
#define FntSetFont(f) GLUE(slotFntSetFont, UInt8 (*)(Int32))(f)
#define KeyCurrentState() GLUE(slotKeyCurrentState, UInt32 (*)(void))()
#define MemPtrFree(p) GLUE(slotMemPtrFree, Err (*)(void*))(p)
#define WinSetTextColor(c) GLUE(slotWinSetTextColor, UInt8 (*)(Int32))(c)
#define TwGetMicroSeconds() GLUE(slotTwGetMicroSeconds, UInt32 (*)(void))()

void** twGlue;
Err twGfxOpenUnsigned(TwGfxType** result, TwGfxInfoType* info);

void* memset(void* to, int value, unsigned long n) {
    UInt8* p = to;
    while (n--) *p++ = (UInt8)value;
    return to;
}
void* memcpy(void* to, const void* from, unsigned long n) {
    UInt8* d = to; const UInt8* s = from;
    while (n--) *d++ = *s++;
    return to;
}

#define FULL_W 320
#define FULL_H 480
#define SMALL 64
#define FULL_RUNS 30
#define SMALL_RUNS 100

static TwGfxType* gfx;
static TwGfxSurfaceType *display, *full, *small;
static UInt16* pixels;

typedef struct { UInt32 mean, min, max; } Times;

static TwGfxSurfaceType* alloc(Int32 w, Int32 h) {
    TwGfxSurfaceInfoType d;
    TwGfxSurfaceType* s = 0;
    d.size = sizeof d; d.width = w; d.height = h; d.rowBytes = 0; d.location = twGfxLocationAcceleratorMemory;
    d.pixelFormat = twGfxPixelFormatRGB565_LE;
    return TwGfxAllocSurface(gfx, &s, &d) ? 0 : s;
}

/* One untimed call first, then RUNS timed ones, each from just before the call to its return. */
static Times measure(TwGfxSurfaceType* s, int runs) {
    Times t;
    UInt32 total = 0, i;
    t.min = 0xffffffffu; t.max = 0;
    TwGfxWriteSurface(s, pixels, 0);
    for (i = 0; i < (UInt32)runs; ++i) {
        UInt32 a = TwGetMicroSeconds(), d;
        TwGfxWriteSurface(s, pixels, 0);
        d = TwGetMicroSeconds() - a;
        total += d;
        if (d < t.min) t.min = d;
        if (d > t.max) t.max = d;
    }
    t.mean = total / runs;
    return t;
}

static UInt16 clip16(UInt32 v) { return v > 0xffff ? 0xffff : (UInt16)v; }

static void showBlocks(UInt16 phase, const Times* f, const Times* s) {
    UInt16 words[10];
    TwGfxSurfaceInfoType info;
    void* address = 0;
    Int32 b, x, y;
    words[0] = 0xf81f; words[1] = 0x07e0; words[2] = phase;
    words[3] = clip16(f->mean); words[4] = clip16(f->min); words[5] = clip16(f->max);
    words[6] = clip16(s->mean); words[7] = clip16(s->min); words[8] = 0; words[9] = 0xf81f;
    info.size = sizeof info;
    if (!display || TwGfxGetSurfaceInfo(display, &info) || TwGfxLockSurface(display, &address) || !address) return;
    for (b = 0; b < 10; ++b)
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 16; ++x) {
                Int32 px = info.width - 40 - (10 - b) * 16 + x, py = info.height - 16 + y;
                ((UInt16*)((UInt8*)address + py * info.rowBytes))[px] = words[b];
            }
    TwGfxUnlockSurface(display, 1);
}

/* "12.34" from microseconds. */
static char* ms(char* p, UInt32 us) {
    char digits[12];
    int n = 0;
    UInt32 whole = us / 1000, frac = (us % 1000) / 10;
    do { digits[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (n) *p++ = digits[--n];
    *p++ = '.'; *p++ = (char)('0' + frac / 10); *p++ = (char)('0' + frac % 10);
    return p;
}
static char* text(char* p, const char* s) { while (*s) *p++ = *s++; return p; }

static void line(int row, const char* s, int n) {
    RectangleType r;
    r.x = 10; r.y = (Int16)(20 + row * 24); r.w = 300; r.h = 22;
    WinPaintRectangle(&r, 0);
    WinSetForeColor(255);
    WinPaintChars(s, n, 14, 22 + row * 24);
    WinSetForeColor(0);
}

/* Two lines per size, kept inside the 320-pixel-wide display window. */
static void report(const char* title, const Times* t, int row) {
    char buf[64], *p = buf;
    p = text(p, title); p = text(p, " mean "); p = ms(p, t->mean);
    line(row, buf, (int)(p - buf));
    p = text(buf, "    min "); p = ms(p, t->min); p = text(p, "  max "); p = ms(p, t->max);
    line(row + 1, buf, (int)(p - buf));
}

static void drawAll(const Times* f, const Times* s) {
    static const char title[] = "TwGfxWriteSurface, ms", hint[] = "Tap: again. Button: quit.";
    WinPushDrawState();
    WinSetCoordinateSystem(0);
    WinSetDrawWindow(WinGetDisplayWindow());
    WinSetBackColor(0);
    WinSetTextColor(255);
    WinSetForeColor(0);
    FntSetFont(1);
    line(0, title, sizeof title - 1);
    report("320x480 x30:", f, 1);
    report("64x64 x100:", s, 3);
    line(5, hint, sizeof hint - 1);
    WinPopDrawState();
}

static void waitPenUp(void) {
    Int16 x, y;
    Boolean down = 1;
    while (down) { EvtGetPenNative(WinGetDisplayWindow(), &x, &y, &down); SysTaskDelay(1); }
}

/* start.s's entry. */
void twgfxTestMain(void** glue) {
    Times f, s;
    TwGfxInfoType info;
    UInt32 i;
    twGlue = glue;
    info.size = sizeof info;
    if (twGfxOpenUnsigned(&gfx, &info) || !gfx) return;
    TwGfxGetDisplaySurface(gfx, &display);
    full = alloc(FULL_W, FULL_H);
    small = alloc(SMALL, SMALL);
    pixels = MemPtrNew(FULL_W * FULL_H * 2);
    memset(&f, 0, sizeof f); memset(&s, 0, sizeof s);
    if (!full || !small || !pixels) {
        showBlocks(5, &f, &s);
        line(0, "Not enough memory.", 18);
        while (!KeyCurrentState()) SysTaskDelay(10);
    } else {
        for (i = 0; i < FULL_W * FULL_H; ++i) pixels[i] = (UInt16)(i * 2654435761u >> 16);
        for (;;) {
            Int16 x, y;
            Boolean down = 0;
            showBlocks(2, &f, &s);
            f = measure(full, FULL_RUNS);
            s = measure(small, SMALL_RUNS);
            showBlocks(3, &f, &s);
            drawAll(&f, &s);
            while (KeyCurrentState()) SysTaskDelay(1);
            for (;;) {
                EvtGetPenNative(WinGetDisplayWindow(), &x, &y, &down);
                if (down || KeyCurrentState()) break;
                SysTaskDelay(2);
            }
            if (!down) break;
            waitPenUp();
        }
    }
    if (pixels) MemPtrFree(pixels);
    if (full) TwGfxFreeSurface(full);
    if (small) TwGfxFreeSurface(small);
    TwGfxClose(gfx);
}
