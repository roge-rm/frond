/* Zodiac TwGfx test program: one call per TwGfx function and option, plus the Window Manager drawing modes
 * and patterns, on small test surfaces. Each run makes one call, picked by a pen tap, so every test starts
 * from the same machine state (tools/zodiac-twgfx/run.py saves one with the program waiting for the pen).
 *
 * The result goes on the screen as a row of 16x16 blocks at the display surface's bottom, 40 pixels from the
 * right (clear of the status bar), each block's RGB565 colour one 16-bit word: f81f, 07e0 (marks), the phase
 * (1 waiting, 3 done, 4 no such test), the test, its return code, the test surface's hash (high, low), 1 if
 * the call changed that surface, the ms from the tap to the result, f81f. The host reads it from a snapshot
 * at the run's end. A call that stops Frond leaves the waiting row. */
#include "twgfx.h"

void** twGlue;
Err twGfxOpenUnsigned(TwGfxType** result, TwGfxInfoType* info);

static void* memSet(void* to, int value, UInt32 n) {
    UInt8* p = to;
    while (n--) *p++ = (UInt8)value;
    return to;
}
/* gcc may call these for struct copies and zeroing. */
void* memset(void* to, int value, unsigned long n) { return memSet(to, value, n); }
void* memcpy(void* to, const void* from, unsigned long n) {
    UInt8* d = to; const UInt8* s = from;
    while (n--) *d++ = *s++;
    return to;
}

/* Test surfaces: 64x64 RGB565 in accelerator memory. */
#define SIZE 64
static TwGfxType* gfx;
static TwGfxSurfaceType *display, *dst, *src, *spare;
static UInt16 pixels[SIZE * SIZE];   /* source data for writes, and the target of reads */
static UInt16 readBack[SIZE * SIZE];
static UInt8 maskBits[SIZE * SIZE / 2];
static UInt16 palette[256];
static WinHandle displayWindow, offscreenWindow;
static UInt8 palmBitmap[16 + 8 + SIZE * SIZE * 2];

static void fillPattern(UInt16* p, UInt32 seed) {
    UInt32 i, v = seed;
    for (i = 0; i < SIZE * SIZE; ++i) { v = v * 1103515245u + 12345u; p[i] = (UInt16)(v >> 16); }
    for (i = 0; i < 64; ++i) p[i * SIZE + (i & 63)] = 0;   /* some black pixels for the colour keys */
}

static void writeByCpu(TwGfxSurfaceType* s, UInt32 seed) {
    void* address = 0;
    if (!s || TwGfxLockSurface(s, &address) || !address) return;
    fillPattern((UInt16*)address, seed);
    TwGfxUnlockSurface(s, 1);
}

static UInt32 surfaceHash(TwGfxSurfaceType* s) {
    void* address = 0;
    UInt32 h = 2166136261u, i;
    if (!s || TwGfxLockSurface(s, &address) || !address) return 0;
    for (i = 0; i < SIZE * SIZE; ++i) h = (h ^ ((UInt16*)address)[i]) * 16777619u;
    TwGfxUnlockSurface(s, 0);
    return h;
}

static void settle(void) {
    int i;
    for (i = 0; i < 50 && dst && TwGfxIsSurfaceReady(dst); ++i) TwGfxWaitForVBlank(gfx);
    for (i = 0; i < 50 && src && TwGfxIsSurfaceReady(src); ++i) TwGfxWaitForVBlank(gfx);
    TwGfxWaitForVBlank(gfx);
}

static TwGfxRectType rect(Int32 x, Int32 y, Int32 w, Int32 h) { TwGfxRectType r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }
static TwGfxPointType point(Int32 x, Int32 y) { TwGfxPointType p; p.x = x; p.y = y; return p; }
static RectangleType winRect(Int16 x, Int16 y, Int16 w, Int16 h) { RectangleType r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }

static Err allocFormat(Int32 format, Int32 location) {
    TwGfxSurfaceInfoType info;
    TwGfxSurfaceType* s = 0;
    Err e;
    info.size = sizeof info; info.width = 32; info.height = 32; info.rowBytes = 0; info.location = location; info.pixelFormat = format;
    e = TwGfxAllocSurface(gfx, &s, &info);
    if (!e && s) TwGfxFreeSurface(s);
    return e;
}

/* A TwGfxBitmapType of the given format over pixels/maskBits with the shared palette. */
static TwGfxBitmapType bitmap(Int32 format, Int32 w, Int32 h) {
    TwGfxBitmapType b;
    Int32 bits = format == twGfxPixelFormat1bpp ? 1 : format == twGfxPixelFormat2bpp ? 2 : format == twGfxPixelFormat4bpp ? 4 :
                 format == twGfxPixelFormat8bpp ? 8 : 16;
    b.size = sizeof b; b.width = w; b.height = h; b.rowBytes = (w * bits + 15) / 16 * 2; b.pixelFormat = format;
    b.data = bits == 16 ? (void*)pixels : (void*)maskBits; b.palette = palette;
    return b;
}

static void put16(UInt8* p, UInt32 v) { p[0] = (UInt8)(v >> 8); p[1] = (UInt8)v; }
/* A Palm OS BitmapType, big-endian like a resource: version 2 at depth 1, 8 or 16, or version 3 with a
 * 16-bit pixel format (2 big-endian, 3 little-endian) at 108 dpi. 32x32. */
static const void* palmBitmapOf(int version, int depth, int pixelFormat) {
    UInt8* b = palmBitmap;
    UInt32 w = 32, h = 32, rowBytes = (w * depth + 15) / 16 * 2, header = version == 3 ? 24 : 16, i;
    memSet(b, 0, sizeof palmBitmap);
    put16(b, w); put16(b + 2, h); put16(b + 4, rowBytes);
    put16(b + 6, depth == 16 && version == 2 ? 0x0400 : 0);   /* directColor */
    b[8] = (UInt8)depth; b[9] = (UInt8)version;
    put16(b + 10, 0); b[12] = 0; b[13] = 0xff;
    if (version == 3) { b[10] = 24; b[11] = (UInt8)pixelFormat; b[13] = 0xff; put16(b + 14, 108); }
    if (version == 2 && depth == 16) { b[16] = 5; b[17] = 6; b[18] = 5; header += 8; }
    for (i = 0; i < rowBytes * h; ++i) b[header + i] = (UInt8)(i * 37 + (i >> 5));
    return b;
}

/* One test: a name and the call. Each returns the call's result (0 for void calls). */
typedef struct { const char* name; Err (*run)(int arg); int arg; } Test;

static Err tInfo(int a) { TwGfxInfoType i; (void)a; i.size = sizeof i; return TwGfxGetInfo(gfx, &i); }
static Err tMemoryUsage(int a) { Int32 used = 0; return TwGfxGetMemoryUsage(gfx, a, &used); }
static Err tDisplaySurface(int a) { TwGfxSurfaceType* s = 0; (void)a; return TwGfxGetDisplaySurface(gfx, &s); }
static Err tPalmDisplaySurface(int a) { TwGfxSurfaceType* s = 0; (void)a; return TwGfxGetPalmDisplaySurface(gfx, &s); }
static Err tInVBlank(int a) { Boolean b = 0; (void)a; return TwGfxInVBlank(gfx, &b); }
static Err tWaitVBlank(int a) { (void)a; return TwGfxWaitForVBlank(gfx); }
static Err tAlloc(int a) { return allocFormat(a, twGfxLocationAcceleratorMemory); }
static Err tAllocLocation(int a) { return allocFormat(twGfxPixelFormatRGB565_LE, a); }
static Err tFree(int a) {
    TwGfxSurfaceInfoType info;
    TwGfxSurfaceType* s = 0;
    Err e;
    (void)a;
    info.size = sizeof info; info.width = 16; info.height = 16; info.rowBytes = 0; info.location = twGfxLocationAcceleratorMemory; info.pixelFormat = twGfxPixelFormatRGB565_LE;
    e = TwGfxAllocSurface(gfx, &s, &info);
    return e ? e : TwGfxFreeSurface(s);
}
static Err tClose(int a) { (void)a; return TwGfxClose(gfx); }
static Err tSurfaceInfo(int a) { TwGfxSurfaceInfoType i; (void)a; i.size = sizeof i; return TwGfxGetSurfaceInfo(dst, &i); }
static Err tSetClip(int a) { TwGfxRectType r = rect(8, 8, 40, 40); return TwGfxSetClip(dst, a ? &r : 0); }
static Err tGetClip(int a) { TwGfxRectType r; (void)a; return TwGfxGetClip(dst, &r); }
static Err tLock(int a) { void* p = 0; Err e = TwGfxLockSurface(dst, &p); if (!e) { ((UInt16*)p)[1] = 0x1234; e = TwGfxUnlockSurface(dst, a); } return e; }
static Err tRead(int a) { return TwGfxReadSurface(dst, readBack, a); }
static Err tReadRegion(int a) { TwGfxRectType r = rect(4, 4, 20, 12); return TwGfxReadSurfaceRegion(dst, &r, readBack, a == 2 ? 64 : 40, a == 1); }
static Err tWrite(int a) { return TwGfxWriteSurface(dst, pixels, a); }
static Err tWriteRegion(int a) { TwGfxRectType r = rect(4, 4, 20, 12); return TwGfxWriteSurfaceRegion(dst, &r, pixels, a == 2 ? 64 : 40, a == 1); }
static Err tReady(int a) { (void)a; return TwGfxIsSurfaceReady(dst); }

/* arg: 0 plain, 1 into the display, 2 overlapping on one surface, 3 clipped at the destination's edges */
static Err tBitBlt(int a) {
    TwGfxPointType p = point(a == 3 ? -8 : 10, a == 3 ? 50 : 6);
    TwGfxRectType r = rect(2, 3, 30, 20);
    return TwGfxBitBlt(a == 1 ? display : dst, &p, a == 2 ? dst : src, &r);
}
static Err tAsyncBlt(int a) { TwGfxPointType p = point(10, 6); TwGfxRectType r = rect(2, 3, 30, 20); (void)a; return TwGfxAsyncBlt(dst, &p, src, &r); }
static Err tTransparent(int a) { TwGfxPointType p = point(10, 6); TwGfxRectType r = rect(2, 3, 30, 20); return TwGfxTransparentBlt(dst, &p, src, &r, a ? 0xff00ff : 0); }
static Err tMaskBlt(int a) { TwGfxPointType p = point(10, 6); TwGfxRectType r = rect(2, 3, 32, 20); TwGfxBitmapType m = bitmap(twGfxPixelFormat1bpp, 32, 20); (void)a; return TwGfxMaskBlt(dst, &p, src, &r, &m); }
static const UInt32 alphas[] = { 0, 0x808080, 0xffffff, 0x204080, 0x404040 };
static Err tBlend(int a) { TwGfxPointType p = point(10, 6); TwGfxRectType r = rect(2, 3, 30, 20); return TwGfxBlendBlt(dst, &p, src, &r, alphas[a]); }
static Err tMaskBlend(int a) { TwGfxPointType p = point(10, 6); TwGfxRectType r = rect(2, 3, 32, 20); TwGfxBitmapType m = bitmap(twGfxPixelFormat4bpp, 32, 20); (void)a; return TwGfxMaskBlendBlt(dst, &p, src, &r, &m); }
/* arg: 0 2x up, 1 half down, 2 an uneven 20x15 to 47x31, 3 into the display */
static Err tStretch(int a) {
    TwGfxRectType d = a == 0 ? rect(0, 0, 60, 40) : a == 1 ? rect(4, 4, 15, 10) : rect(3, 5, 47, 31);
    TwGfxRectType s = a == 0 ? rect(2, 2, 30, 20) : a == 1 ? rect(0, 0, 30, 20) : rect(1, 1, 20, 15);
    return TwGfxStretchBlt(a == 3 ? display : dst, &d, src, &s);
}
/* arg: the flags; 0x100 set means the uneven ratio. */
static Err tStretch2(int a) {
    TwGfxRectType d = (a & 0x100) ? rect(3, 5, 47, 31) : rect(0, 0, 60, 40);
    TwGfxRectType s = (a & 0x100) ? rect(1, 1, 20, 15) : rect(2, 2, 30, 20);
    return TwGfxStretchBlt2(dst, &d, src, &s, a & 0xff);
}
static Err tTile(int a) { TwGfxRectType d = rect(2, 2, 60, 58); TwGfxPointType p = point(a ? 5 : 0, a ? 3 : 0); return TwGfxTileBlt(dst, &d, spare, &p); }
static Err tTransform(int a) { TwGfxPointType p = point(10, 6); TwGfxRectType r = rect(2, 3, 30, 20); return TwGfxTransformBlt(dst, &p, src, &r, a >> 2, a & 3); }
static const TwGfxPointType points[] = { {3, 3}, {60, 5}, {30, 40}, {-5, 20}, {70, 70}, {10, 60} };
static const UInt32 colours[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x808080, 0x123456 };
static Err tPoints(int a) { (void)a; return TwGfxDrawPoints(dst, points, 6, 0x40c0ff); }
static Err tColorPoints(int a) { (void)a; return TwGfxDrawColorPoints(dst, points, 6, colours); }
static Err tLines(int a) { return TwGfxDrawLines(dst, points, a ? 6 : 2, 0xffc040); }
static Err tSegments(int a) { (void)a; return TwGfxDrawLineSegments(dst, points, 6, 0x20ff20); }
static Err tDrawRect(int a) { TwGfxRectType r = a ? rect(-4, 50, 30, 30) : rect(5, 6, 40, 30); return TwGfxDrawRect(dst, &r, 0xff2080); }
static Err tFillRect(int a) { TwGfxRectType r = a == 1 ? rect(-4, 50, 30, 30) : rect(5, 6, 40, 30); return TwGfxFillRect(a == 2 ? display : dst, &r, 0x2080ff); }
static Err tSpans(int a) { static const TwGfxSpanType spans[] = { {2, 2, 40}, {-4, 10, 20}, {50, 20, 30}, {5, 63, 10} }; (void)a; return TwGfxDrawSpans(dst, spans, 4, 0xa0a0a0); }
static Err tClipFill(int a) { TwGfxRectType c = rect(8, 8, 40, 40), r = rect(0, 0, 64, 64); Err e; (void)a; TwGfxSetClip(dst, &c); e = TwGfxFillRect(dst, &r, 0x40ff40); TwGfxSetClip(dst, 0); return e; }
static Err tClipLines(int a) { TwGfxRectType c = rect(8, 8, 40, 40); Err e; (void)a; TwGfxSetClip(dst, &c); e = TwGfxDrawLines(dst, points, 6, 0xff4040); TwGfxSetClip(dst, 0); return e; }
static Err tDrawBitmap(int a) { TwGfxPointType p = point(6, 7); TwGfxBitmapType b = bitmap(a, 32, 24); return TwGfxDrawBitmap(dst, &p, &b); }
/* arg: version * 100 + depth, or 300 + 16 + 1000 * pixel format for version 3 */
static Err tPalmBitmap(int a) { TwGfxPointType p = point(6, 7); return TwGfxDrawPalmBitmap(dst, &p, palmBitmapOf((a % 1000) / 100, a % 100, a / 1000)); }

/* Window Manager drawing on the display window, 64x64 at the top left. */
static Err winBegin(void) { WinPushDrawState(); WinSetDrawWindow(displayWindow); WinSetForeColor(0x23); WinSetBackColor(0x7d); return 0; }
static Err winEnd(void) { WinPopDrawState(); return 0; }
static Err tWinRect(int a) { RectangleType r = winRect(4, 4, 40, 30); winBegin(); WinSetDrawMode(a); WinPaintRectangle(&r, 0); return winEnd(); }
static Err tWinRound(int a) { RectangleType r = winRect(4, 4, 40, 30); (void)a; winBegin(); WinPaintRectangle(&r, 6); return winEnd(); }
static Err tWinPattern(int a) {
    static const UInt8 custom[8] = { 0xaa, 0x55, 0xcc, 0x33, 0xf0, 0x0f, 0x81, 0x18 };
    RectangleType r = winRect(4, 4, 40, 30);
    winBegin();
    if (a == 3) WinSetPattern(custom); else WinSetPatternType(a);
    WinPaintRectangle(&r, 0);
    return winEnd();
}
/* arg: draw mode * 10 + pattern type; erase, mask and overlay with a pattern reach the transparent 8x8 brush
 * (2008a048-2008a0e0). */
static Err tWinPatternMode(int a) {
    RectangleType r = winRect(4, 4, 40, 30);
    winBegin();
    WinSetDrawMode(a / 10);
    WinSetPatternType(a % 10);
    WinPaintRectangle(&r, 0);
    return winEnd();
}
/* arg: 0 WinPaintLine, 1 WinPaintLines, with the gray pattern. */
static Err tWinPatternLine(int a) {
    static const Int16 lines[8] = { 2, 3, 50, 40, 60, 2, 4, 50 };
    winBegin();
    WinSetPatternType(2);
    if (a) WinPaintLines(2, lines); else WinPaintLine(2, 3, 50, 40);
    return winEnd();
}
static Err tWinLine(int a) { winBegin(); WinSetDrawMode(a); WinPaintLine(2, 3, 50, 40); return winEnd(); }
static Err tWinPixel(int a) { (void)a; winBegin(); WinPaintPixel(9, 9); return winEnd(); }
static Err tWinChars(int a) { winBegin(); WinSetDrawMode(a); WinPaintChars("TwGfx", 5, 4, 4); return winEnd(); }
static Err tWinFrame(int a) { RectangleType r = winRect(6, 6, 40, 30); (void)a; winBegin(); WinPaintRectangleFrame(1, &r); WinPaintRoundedRectangleFrame(&r, 1, 4, 2); return winEnd(); }
static Err tWinBitmap(int a) { winBegin(); WinSetDrawMode(a % 10); WinPaintBitmap(palmBitmapOf(2, a / 10 ? 16 : 8, 0), 4, 4); return winEnd(); }
static Err tWinCopy(int a) {
    RectangleType r = winRect(0, 0, 30, 20);
    winBegin();
    WinCopyRectangle(a >= 10 ? offscreenWindow : displayWindow, displayWindow, &r, 8, 9, a % 10);
    return winEnd();
}
static Err tWinScroll(int a) { RectangleType r = winRect(2, 2, 50, 40), v; winBegin(); WinScrollRectangle(&r, a, 5, &v); return winEnd(); }
static Err tWinSaveBits(int a) {
    RectangleType r = winRect(2, 2, 40, 30);
    UInt16 e = 0;
    WinHandle w;
    (void)a;
    winBegin();
    w = WinSaveBits(&r, &e);
    if (w) WinRestoreBits(w, 6, 6);
    winEnd();
    return e;
}
static Err tWinNative(int a) {
    RectangleType r = winRect(4, 4, 40, 30);
    UInt16 old;
    winBegin();
    old = WinSetCoordinateSystem(a);
    WinPaintRectangle(&r, 0);
    WinCopyRectangle(displayWindow, displayWindow, &r, 10, 12, 0);
    WinSetCoordinateSystem(old);
    return winEnd();
}

static const Test tests[] = {
    {"get-info", tInfo, 0},
    {"get-memory-usage accelerator", tMemoryUsage, 1},
    {"get-display-surface", tDisplaySurface, 0},
    {"get-palm-display-surface", tPalmDisplaySurface, 0},
    {"in-vblank", tInVBlank, 0},
    {"wait-for-vblank", tWaitVBlank, 0},
    {"alloc-surface 1bpp", tAlloc, twGfxPixelFormat1bpp},
    {"alloc-surface rgb565-le", tAlloc, twGfxPixelFormatRGB565_LE},
    {"alloc-surface rgb565-be", tAlloc, twGfxPixelFormatRGB565_BE},
    {"alloc-surface 2bpp", tAlloc, twGfxPixelFormat2bpp},
    {"alloc-surface 4bpp", tAlloc, twGfxPixelFormat4bpp},
    {"alloc-surface 8bpp", tAlloc, twGfxPixelFormat8bpp},
    {"alloc-surface location 0", tAllocLocation, 0},
    {"alloc-surface location 2", tAllocLocation, 2},
    {"get-surface-info", tSurfaceInfo, 0},
    {"set-clip", tSetClip, 1},
    {"get-clip", tGetClip, 0},
    {"set-clip null", tSetClip, 0},
    {"lock-unlock update", tLock, 1},
    {"lock-unlock no-update", tLock, 0},
    {"read-surface", tRead, 0},
    {"read-surface async", tRead, 1},
    {"read-surface-region", tReadRegion, 0},
    {"read-surface-region async", tReadRegion, 1},
    {"read-surface-region wide-rows", tReadRegion, 2},
    {"write-surface", tWrite, 0},
    {"write-surface async", tWrite, 1},
    {"write-surface-region", tWriteRegion, 0},
    {"write-surface-region async", tWriteRegion, 1},
    {"write-surface-region wide-rows", tWriteRegion, 2},
    {"is-surface-ready", tReady, 0},
    {"bitblt", tBitBlt, 0},
    {"bitblt to-display", tBitBlt, 1},
    {"bitblt overlap", tBitBlt, 2},
    {"bitblt clipped", tBitBlt, 3},
    {"async-blt", tAsyncBlt, 0},
    {"transparent-blt key-000000", tTransparent, 0},
    {"transparent-blt key-ff00ff", tTransparent, 1},
    {"mask-blt 1bpp", tMaskBlt, 0},
    {"blend-blt alpha-000000", tBlend, 0},
    {"blend-blt alpha-808080", tBlend, 1},
    {"blend-blt alpha-ffffff", tBlend, 2},
    {"blend-blt alpha-204080", tBlend, 3},
    {"blend-blt alpha-404040", tBlend, 4},
    {"mask-blend-blt 4bpp", tMaskBlend, 0},
    {"stretch-blt 2x", tStretch, 0},
    {"stretch-blt half", tStretch, 1},
    {"stretch-blt uneven", tStretch, 2},
    {"stretch-blt to-display", tStretch, 3},
    {"stretch-blt2 flags-0", tStretch2, 0},
    {"stretch-blt2 flags-1", tStretch2, 1},
    {"stretch-blt2 flags-2", tStretch2, 2},
    {"stretch-blt2 flags-4", tStretch2, 4},
    {"stretch-blt2 flags-5", tStretch2, 5},
    {"stretch-blt2 flags-3", tStretch2, 3},
    {"stretch-blt2 flags-8", tStretch2, 8},
    {"stretch-blt2 uneven flags-0", tStretch2, 0x100},
    {"stretch-blt2 uneven flags-1", tStretch2, 0x101},
    {"stretch-blt2 uneven flags-2", tStretch2, 0x102},
    {"stretch-blt2 uneven flags-4", tStretch2, 0x104},
    {"stretch-blt2 uneven flags-5", tStretch2, 0x105},
    {"tile-blt", tTile, 0},
    {"tile-blt aligned-5-3", tTile, 1},
    {"transform-blt rot-0 mirror-0", tTransform, 0},
    {"transform-blt rot-0 mirror-1", tTransform, 1},
    {"transform-blt rot-0 mirror-2", tTransform, 2},
    {"transform-blt rot-0 mirror-3", tTransform, 3},
    {"transform-blt rot-1 mirror-0", tTransform, 4},
    {"transform-blt rot-1 mirror-1", tTransform, 5},
    {"transform-blt rot-1 mirror-2", tTransform, 6},
    {"transform-blt rot-1 mirror-3", tTransform, 7},
    {"transform-blt rot-2 mirror-0", tTransform, 8},
    {"transform-blt rot-2 mirror-1", tTransform, 9},
    {"transform-blt rot-2 mirror-2", tTransform, 10},
    {"transform-blt rot-2 mirror-3", tTransform, 11},
    {"transform-blt rot-3 mirror-0", tTransform, 12},
    {"transform-blt rot-3 mirror-1", tTransform, 13},
    {"transform-blt rot-3 mirror-2", tTransform, 14},
    {"transform-blt rot-3 mirror-3", tTransform, 15},
    {"draw-points", tPoints, 0},
    {"draw-color-points", tColorPoints, 0},
    {"draw-lines 2", tLines, 0},
    {"draw-lines 6", tLines, 1},
    {"draw-line-segments", tSegments, 0},
    {"draw-rect", tDrawRect, 0},
    {"draw-rect clipped", tDrawRect, 1},
    {"fill-rect", tFillRect, 0},
    {"fill-rect clipped", tFillRect, 1},
    {"fill-rect display", tFillRect, 2},
    {"draw-spans", tSpans, 0},
    {"set-clip fill-rect", tClipFill, 0},
    {"set-clip draw-lines", tClipLines, 0},
    {"draw-bitmap 1bpp", tDrawBitmap, twGfxPixelFormat1bpp},
    {"draw-bitmap rgb565-le", tDrawBitmap, twGfxPixelFormatRGB565_LE},
    {"draw-bitmap rgb565-be", tDrawBitmap, twGfxPixelFormatRGB565_BE},
    {"draw-bitmap 2bpp", tDrawBitmap, twGfxPixelFormat2bpp},
    {"draw-bitmap 4bpp", tDrawBitmap, twGfxPixelFormat4bpp},
    {"draw-bitmap 8bpp", tDrawBitmap, twGfxPixelFormat8bpp},
    {"draw-palm-bitmap v2 1bpp", tPalmBitmap, 201},
    {"draw-palm-bitmap v2 8bpp", tPalmBitmap, 208},
    {"draw-palm-bitmap v2 16bpp", tPalmBitmap, 216},
    {"draw-palm-bitmap v3 rgb565-be 108dpi", tPalmBitmap, 2316},
    {"draw-palm-bitmap v3 rgb565-le 108dpi", tPalmBitmap, 3316},
    {"win-paint-rect paint", tWinRect, 0},
    {"win-paint-rect erase", tWinRect, 1},
    {"win-paint-rect mask", tWinRect, 2},
    {"win-paint-rect invert", tWinRect, 3},
    {"win-paint-rect overlay", tWinRect, 4},
    {"win-paint-rect paint-inverse", tWinRect, 5},
    {"win-paint-rect swap", tWinRect, 6},
    {"win-paint-rect rounded", tWinRound, 0},
    {"win-pattern black", tWinPattern, 0},
    {"win-pattern white", tWinPattern, 1},
    {"win-pattern gray", tWinPattern, 2},
    {"win-pattern custom", tWinPattern, 3},
    {"win-pattern light-gray", tWinPattern, 4},
    {"win-pattern dark-gray", tWinPattern, 5},
    {"win-paint-line paint", tWinLine, 0},
    {"win-paint-line invert", tWinLine, 3},
    {"win-paint-pixel", tWinPixel, 0},
    {"win-paint-chars paint", tWinChars, 0},
    {"win-paint-chars overlay", tWinChars, 4},
    {"win-paint-chars swap", tWinChars, 6},
    {"win-paint-frame", tWinFrame, 0},
    {"win-paint-bitmap 8bpp paint", tWinBitmap, 0},
    {"win-paint-bitmap 8bpp erase", tWinBitmap, 1},
    {"win-paint-bitmap 8bpp mask", tWinBitmap, 2},
    {"win-paint-bitmap 8bpp invert", tWinBitmap, 3},
    {"win-paint-bitmap 8bpp overlay", tWinBitmap, 4},
    {"win-paint-bitmap 8bpp paint-inverse", tWinBitmap, 5},
    {"win-paint-bitmap 8bpp swap", tWinBitmap, 6},
    {"win-paint-bitmap 16bpp paint", tWinBitmap, 10},
    {"win-paint-bitmap 16bpp overlay", tWinBitmap, 14},
    {"win-copy-rect display paint", tWinCopy, 0},
    {"win-copy-rect display erase", tWinCopy, 1},
    {"win-copy-rect display mask", tWinCopy, 2},
    {"win-copy-rect display invert", tWinCopy, 3},
    {"win-copy-rect display overlay", tWinCopy, 4},
    {"win-copy-rect display paint-inverse", tWinCopy, 5},
    {"win-copy-rect display swap", tWinCopy, 6},
    {"win-copy-rect offscreen paint", tWinCopy, 10},
    {"win-copy-rect offscreen overlay", tWinCopy, 14},
    {"win-copy-rect offscreen swap", tWinCopy, 16},
    {"win-scroll-rect up", tWinScroll, 0},
    {"win-scroll-rect down", tWinScroll, 1},
    {"win-scroll-rect left", tWinScroll, 2},
    {"win-scroll-rect right", tWinScroll, 3},
    {"win-save-restore-bits", tWinSaveBits, 0},
    {"win-native-coords", tWinNative, 0},
    {"win-coords 108", tWinNative, 108},
    {"win-coords 144", tWinNative, 144},
    {"free-surface", tFree, 0},
    {"close", tClose, 0},
    {"win-pattern gray erase", tWinPatternMode, 12},
    {"win-pattern gray mask", tWinPatternMode, 22},
    {"win-pattern gray invert", tWinPatternMode, 32},
    {"win-pattern gray overlay", tWinPatternMode, 42},
    {"win-pattern gray paint-inverse", tWinPatternMode, 52},
    {"win-pattern-line paint", tWinPatternLine, 0},
    {"win-pattern-line lines", tWinPatternLine, 1},
};
#define TEST_COUNT ((int)(sizeof tests / sizeof tests[0]))

/* The first test to run comes from a pen tap, so one saved state of the program waiting here serves every
 * run: native coordinates, cells 20 pixels square, index = x / 20 + 20 * (y / 20). */
#define CELL 20
#define COLUMNS 20
static int startIndex(Int16* x, Int16* y) {
    Boolean down = 0;
    for (;;) {
        EvtGetPenNative(WinGetDisplayWindow(), x, y, &down);
        if (down) break;
        SysTaskDelay(1);
    }
    return *x / CELL + COLUMNS * (*y / CELL);
}

#define BLOCK 16
#define BLOCKS 10
#define MARGIN 40   /* clear of the status bar */
static void showResult(UInt16 phase, UInt16 test, UInt16 ret, UInt32 sum, UInt16 changed, UInt16 ms) {
    UInt16 words[BLOCKS];
    TwGfxSurfaceInfoType info;
    void* address = 0;
    Int32 b, x, y;
    words[0] = 0xf81f; words[1] = 0x07e0; words[2] = phase; words[3] = test; words[4] = ret;
    words[5] = (UInt16)(sum >> 16); words[6] = (UInt16)sum; words[7] = changed; words[8] = ms; words[9] = 0xf81f;
    info.size = sizeof info;
    if (!display || TwGfxGetSurfaceInfo(display, &info) || TwGfxLockSurface(display, &address) || !address) return;
    for (b = 0; b < BLOCKS; ++b)
        for (y = 0; y < BLOCK; ++y)
            for (x = 0; x < BLOCK; ++x) {
                Int32 px = info.width - MARGIN - (BLOCKS - b) * BLOCK + x, py = info.height - BLOCK + y;
                ((UInt16*)((UInt8*)address + py * info.rowBytes))[px] = words[b];
            }
    TwGfxUnlockSurface(display, 1);
}

static void openGfx(void) {
    TwGfxInfoType info;
    info.size = sizeof info;
    gfx = 0; display = 0;
    if (twGfxOpenUnsigned(&gfx, &info) || !gfx) return;
    TwGfxGetDisplaySurface(gfx, &display);
}

static void setup(void) {
    TwGfxSurfaceInfoType d;
    UInt16 e = 0;
    UInt32 i;
    d.size = sizeof d; d.width = SIZE; d.height = SIZE; d.rowBytes = 0; d.location = twGfxLocationAcceleratorMemory; d.pixelFormat = twGfxPixelFormatRGB565_LE;
    TwGfxAllocSurface(gfx, &dst, &d);
    TwGfxAllocSurface(gfx, &src, &d);
    d.width = 16; d.height = 16;
    TwGfxAllocSurface(gfx, &spare, &d);
    fillPattern(pixels, 7);
    for (i = 0; i < sizeof maskBits; ++i) maskBits[i] = (UInt8)(i * 29 + 0x5a);
    for (i = 0; i < 256; ++i) palette[i] = (UInt16)(i * 0x0841u + (i << 8));
    displayWindow = WinGetDisplayWindow();
    offscreenWindow = WinCreateOffscreenWindow(64, 64, 2, &e);
    writeByCpu(dst, 11);
    writeByCpu(src, 23);
    writeByCpu(spare, 31);
    settle();
}

void twgfxTestMain(void** glue) {
    Int16 x = 0, y = 0;
    UInt32 tapped, before, after;
    UInt16 ret, phase = 3;
    int n;
    twGlue = glue;
    openGfx();
    showResult(1, 0xffff, 0, 0, 0, 0);
    n = startIndex(&x, &y);
    tapped = TimGetTicks();
    setup();
    before = surfaceHash(dst);
    if (n < 0 || n >= TEST_COUNT) { phase = 4; ret = 0; }
    else ret = tests[n].run(tests[n].arg);
    /* Close freed the surfaces; open again for the result. */
    if (n >= 0 && n < TEST_COUNT && tests[n].run == tClose) { dst = src = spare = 0; openGfx(); }
    settle();
    after = surfaceHash(dst);
    showResult(phase, (UInt16)n, ret, after, after != before, (UInt16)(TimGetTicks() - tapped));
    /* Stay here, drawing nothing, until the host's snapshot. */
    for (;;) SysTaskDelay(100);
}
