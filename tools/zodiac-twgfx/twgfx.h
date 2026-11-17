/* Minimal declarations for the Zodiac TwGlue calls the TwGfx test program makes. There are no Tapwave SDK
 * headers here: slot numbers are from core/src/uarm/uarm/twglue_names.inc (index = line - 3), argument
 * shapes from the public TwGfx API (hostile-takeover's inc/TwGfx.h) and the TwGlue logs of the game pass.
 * Calls go through the table the OS passes in r1, as the games do. */
#ifndef ZODIAC_TWGFX_H
#define ZODIAC_TWGFX_H

typedef signed char Int8;
typedef unsigned char UInt8;
typedef short Int16;
typedef unsigned short UInt16;
typedef long Int32;
typedef unsigned long UInt32;
typedef UInt16 Err;
typedef UInt8 Boolean;

typedef struct { Int32 size, displayWidth, displayHeight, displayRowBytes, displayPixelFormat, freeAcceleratorMemory, totalAcceleratorMemory; } TwGfxInfoType;
typedef struct { Int32 size, width, height, rowBytes, location, pixelFormat; } TwGfxSurfaceInfoType;
typedef struct { Int32 size, width, height, rowBytes, pixelFormat; void* data; UInt16* palette; } TwGfxBitmapType;
typedef struct { Int32 x, y; } TwGfxPointType;
typedef struct { Int32 x, y, w, h; } TwGfxRectType;
typedef struct { Int32 x, y, w; } TwGfxSpanType;
typedef UInt32 TwGfxPackedRGBType;  /* 0x00rrggbb */
typedef struct TwGfxTag TwGfxType;
typedef struct TwGfxSurfaceTag TwGfxSurfaceType;

typedef struct { Int16 x, y, w, h; } RectangleType;
typedef void* WinHandle;

#define twGfxLocationAcceleratorMemory 1
#define twGfxPixelFormat1bpp 0
#define twGfxPixelFormatRGB565_LE 1
#define twGfxPixelFormatRGB565_BE 2
#define twGfxPixelFormat2bpp 3
#define twGfxPixelFormat4bpp 4
#define twGfxPixelFormat8bpp 5

/* TwGlue slots */
#define slotEvtGetPenNative 554
#define slotMemPtrNew 137
#define slotSysTaskDelay 161
#define slotTimGetTicks 164
#define slotWinCopyRectangle 250
#define slotWinCreateOffscreenWindow 251
#define slotWinDeleteWindow 253
#define slotWinPaintBitmap 263
#define slotWinPaintChars 265
#define slotWinPaintLine 266
#define slotWinPaintPixel 268
#define slotWinPaintRectangle 270
#define slotWinPaintRectangleFrame 271
#define slotWinPaintRoundedRectangleFrame 272
#define slotWinPushDrawState 273
#define slotWinPopDrawState 274
#define slotWinRestoreBits 275
#define slotWinSaveBits 276
#define slotWinScrollRectangle 281
#define slotWinGetDisplayWindow 284
#define slotWinSetDrawWindow 288
#define slotWinSetCoordinateSystem 293
#define slotWinSetBackColor 294
#define slotWinSetForeColor 296
#define slotWinSetDrawMode 300
#define slotWinSetPattern 302
#define slotWinSetPatternType 303
#define slotTwGfxClose 351
#define slotTwGfxGetInfo 352
#define slotTwGfxGetMemoryUsage 353
#define slotTwGfxGetDisplaySurface 354
#define slotTwGfxInVBlank 355
#define slotTwGfxWaitForVBlank 356
#define slotTwGfxAllocSurface 357
#define slotTwGfxFreeSurface 358
#define slotTwGfxSetClip 359
#define slotTwGfxGetClip 360
#define slotTwGfxGetSurfaceInfo 361
#define slotTwGfxLockSurface 362
#define slotTwGfxUnlockSurface 363
#define slotTwGfxReadSurface 364
#define slotTwGfxWriteSurface 365
#define slotTwGfxIsSurfaceReady 366
#define slotTwGfxBitBlt 367
#define slotTwGfxTransformBlt 368
#define slotTwGfxDrawPoints 369
#define slotTwGfxDrawColorPoints 370
#define slotTwGfxDrawLines 371
#define slotTwGfxDrawLineSegments 372
#define slotTwGfxDrawRect 373
#define slotTwGfxFillRect 374
#define slotTwGfxDrawSpans 375
#define slotTwGfxDrawBitmap 376
#define slotTwGfxGetPalmDisplaySurface 380
#define slotTwGfxReadSurfaceRegion 381
#define slotTwGfxWriteSurfaceRegion 382
#define slotTwGfxAsyncBlt 383
#define slotTwGfxTransparentBlt 384
#define slotTwGfxMaskBlt 385
#define slotTwGfxBlendBlt 386
#define slotTwGfxMaskBlendBlt 387
#define slotTwGfxStretchBlt 388
#define slotTwGfxTileBlt 389
#define slotTwGfxDrawPalmBitmap 463
#define slotTwGfxStretchBlt2 580

extern void** twGlue;
#define GLUE(slot, type) ((type)twGlue[slot])

#define EvtGetPenNative(w, x, y, d) GLUE(slotEvtGetPenNative, void (*)(WinHandle, Int16*, Int16*, Boolean*))(w, x, y, d)
#define MemPtrNew(n) GLUE(slotMemPtrNew, void* (*)(UInt32))(n)
#define SysTaskDelay(t) GLUE(slotSysTaskDelay, Err (*)(Int32))(t)
#define TimGetTicks() GLUE(slotTimGetTicks, UInt32 (*)(void))()

#define WinCopyRectangle(s, d, r, x, y, m) GLUE(slotWinCopyRectangle, void (*)(WinHandle, WinHandle, const RectangleType*, Int32, Int32, Int32))(s, d, r, x, y, m)
#define WinCreateOffscreenWindow(w, h, f, e) GLUE(slotWinCreateOffscreenWindow, WinHandle (*)(Int32, Int32, Int32, UInt16*))(w, h, f, e)
#define WinDeleteWindow(w, e) GLUE(slotWinDeleteWindow, void (*)(WinHandle, Int32))(w, e)
#define WinPaintBitmap(b, x, y) GLUE(slotWinPaintBitmap, void (*)(const void*, Int32, Int32))(b, x, y)
#define WinPaintChars(s, n, x, y) GLUE(slotWinPaintChars, void (*)(const char*, Int32, Int32, Int32))(s, n, x, y)
#define WinPaintLine(a, b, c, d) GLUE(slotWinPaintLine, void (*)(Int32, Int32, Int32, Int32))(a, b, c, d)
#define WinPaintPixel(x, y) GLUE(slotWinPaintPixel, void (*)(Int32, Int32))(x, y)
#define WinPaintRectangle(r, c) GLUE(slotWinPaintRectangle, void (*)(const RectangleType*, Int32))(r, c)
#define WinPaintRectangleFrame(f, r) GLUE(slotWinPaintRectangleFrame, void (*)(Int32, const RectangleType*))(f, r)
#define WinPaintRoundedRectangleFrame(r, w, c, s) GLUE(slotWinPaintRoundedRectangleFrame, void (*)(const RectangleType*, Int32, Int32, Int32))(r, w, c, s)
#define WinPushDrawState() GLUE(slotWinPushDrawState, void (*)(void))()
#define WinPopDrawState() GLUE(slotWinPopDrawState, void (*)(void))()
#define WinRestoreBits(w, x, y) GLUE(slotWinRestoreBits, void (*)(WinHandle, Int32, Int32))(w, x, y)
#define WinSaveBits(r, e) GLUE(slotWinSaveBits, WinHandle (*)(const RectangleType*, UInt16*))(r, e)
#define WinScrollRectangle(r, d, n, v) GLUE(slotWinScrollRectangle, void (*)(const RectangleType*, Int32, Int32, RectangleType*))(r, d, n, v)
#define WinGetDisplayWindow() GLUE(slotWinGetDisplayWindow, WinHandle (*)(void))()
#define WinSetDrawWindow(w) GLUE(slotWinSetDrawWindow, WinHandle (*)(WinHandle))(w)
#define WinSetCoordinateSystem(c) GLUE(slotWinSetCoordinateSystem, UInt16 (*)(Int32))(c)
#define WinSetBackColor(c) GLUE(slotWinSetBackColor, UInt8 (*)(Int32))(c)
#define WinSetForeColor(c) GLUE(slotWinSetForeColor, UInt8 (*)(Int32))(c)
#define WinSetDrawMode(m) GLUE(slotWinSetDrawMode, Int32 (*)(Int32))(m)
#define WinSetPattern(p) GLUE(slotWinSetPattern, void (*)(const UInt8*))(p)
#define WinSetPatternType(p) GLUE(slotWinSetPatternType, void (*)(Int32))(p)

#define TwGfxClose(g) GLUE(slotTwGfxClose, Err (*)(TwGfxType*))(g)
#define TwGfxGetInfo(g, i) GLUE(slotTwGfxGetInfo, Err (*)(TwGfxType*, TwGfxInfoType*))(g, i)
#define TwGfxGetMemoryUsage(g, l, u) GLUE(slotTwGfxGetMemoryUsage, Err (*)(TwGfxType*, Int32, Int32*))(g, l, u)
#define TwGfxGetDisplaySurface(g, s) GLUE(slotTwGfxGetDisplaySurface, Err (*)(TwGfxType*, TwGfxSurfaceType**))(g, s)
#define TwGfxGetPalmDisplaySurface(g, s) GLUE(slotTwGfxGetPalmDisplaySurface, Err (*)(TwGfxType*, TwGfxSurfaceType**))(g, s)
#define TwGfxInVBlank(g, b) GLUE(slotTwGfxInVBlank, Err (*)(TwGfxType*, Boolean*))(g, b)
#define TwGfxWaitForVBlank(g) GLUE(slotTwGfxWaitForVBlank, Err (*)(TwGfxType*))(g)
#define TwGfxAllocSurface(g, s, d) GLUE(slotTwGfxAllocSurface, Err (*)(TwGfxType*, TwGfxSurfaceType**, TwGfxSurfaceInfoType*))(g, s, d)
#define TwGfxFreeSurface(s) GLUE(slotTwGfxFreeSurface, Err (*)(TwGfxSurfaceType*))(s)
#define TwGfxSetClip(s, r) GLUE(slotTwGfxSetClip, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*))(s, r)
#define TwGfxGetClip(s, r) GLUE(slotTwGfxGetClip, Err (*)(TwGfxSurfaceType*, TwGfxRectType*))(s, r)
#define TwGfxGetSurfaceInfo(s, i) GLUE(slotTwGfxGetSurfaceInfo, Err (*)(TwGfxSurfaceType*, TwGfxSurfaceInfoType*))(s, i)
#define TwGfxLockSurface(s, a) GLUE(slotTwGfxLockSurface, Err (*)(TwGfxSurfaceType*, void**))(s, a)
#define TwGfxUnlockSurface(s, u) GLUE(slotTwGfxUnlockSurface, Err (*)(TwGfxSurfaceType*, Int32))(s, u)
#define TwGfxReadSurface(s, p, a) GLUE(slotTwGfxReadSurface, Err (*)(TwGfxSurfaceType*, void*, Int32))(s, p, a)
#define TwGfxReadSurfaceRegion(s, r, p, rb, a) GLUE(slotTwGfxReadSurfaceRegion, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*, void*, UInt32, Int32))(s, r, p, rb, a)
#define TwGfxWriteSurface(s, p, a) GLUE(slotTwGfxWriteSurface, Err (*)(TwGfxSurfaceType*, const void*, Int32))(s, p, a)
#define TwGfxWriteSurfaceRegion(s, r, p, rb, a) GLUE(slotTwGfxWriteSurfaceRegion, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*, const void*, UInt32, Int32))(s, r, p, rb, a)
#define TwGfxIsSurfaceReady(s) GLUE(slotTwGfxIsSurfaceReady, Err (*)(TwGfxSurfaceType*))(s)
/* The blits: destination, destination point or rect, source, source rect or point, then extras. */
typedef Err (*BlitFn)(TwGfxSurfaceType*, const TwGfxPointType*, TwGfxSurfaceType*, const TwGfxRectType*);
typedef Err (*BlitArgFn)(TwGfxSurfaceType*, const TwGfxPointType*, TwGfxSurfaceType*, const TwGfxRectType*, UInt32);
#define TwGfxBitBlt(d, p, s, r) GLUE(slotTwGfxBitBlt, BlitFn)(d, p, s, r)
#define TwGfxAsyncBlt(d, p, s, r) GLUE(slotTwGfxAsyncBlt, BlitFn)(d, p, s, r)
#define TwGfxTransparentBlt(d, p, s, r, c) GLUE(slotTwGfxTransparentBlt, BlitArgFn)(d, p, s, r, c)
#define TwGfxMaskBlt(d, p, s, r, m) GLUE(slotTwGfxMaskBlt, BlitArgFn)(d, p, s, r, (UInt32)(m))
#define TwGfxBlendBlt(d, p, s, r, a) GLUE(slotTwGfxBlendBlt, BlitArgFn)(d, p, s, r, a)
#define TwGfxMaskBlendBlt(d, p, s, r, m) GLUE(slotTwGfxMaskBlendBlt, BlitArgFn)(d, p, s, r, (UInt32)(m))
#define TwGfxTransformBlt(d, p, s, r, rot, mir) GLUE(slotTwGfxTransformBlt, Err (*)(TwGfxSurfaceType*, const TwGfxPointType*, TwGfxSurfaceType*, const TwGfxRectType*, Int32, Int32))(d, p, s, r, rot, mir)
#define TwGfxStretchBlt(d, dr, s, sr) GLUE(slotTwGfxStretchBlt, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*, TwGfxSurfaceType*, const TwGfxRectType*))(d, dr, s, sr)
/* Flags 1, 2, 4 per the ROM (plan, "Z8b b3 stretch facts"). */
#define TwGfxStretchBlt2(d, dr, s, sr, f) GLUE(slotTwGfxStretchBlt2, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*, TwGfxSurfaceType*, const TwGfxRectType*, Int32))(d, dr, s, sr, f)
#define TwGfxTileBlt(d, dr, s, p) GLUE(slotTwGfxTileBlt, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*, TwGfxSurfaceType*, const TwGfxPointType*))(d, dr, s, p)
#define TwGfxDrawPoints(d, p, n, c) GLUE(slotTwGfxDrawPoints, Err (*)(TwGfxSurfaceType*, const TwGfxPointType*, Int32, UInt32))(d, p, n, c)
#define TwGfxDrawColorPoints(d, p, n, c) GLUE(slotTwGfxDrawColorPoints, Err (*)(TwGfxSurfaceType*, const TwGfxPointType*, Int32, const UInt32*))(d, p, n, c)
#define TwGfxDrawLines(d, p, n, c) GLUE(slotTwGfxDrawLines, Err (*)(TwGfxSurfaceType*, const TwGfxPointType*, Int32, UInt32))(d, p, n, c)
#define TwGfxDrawLineSegments(d, p, n, c) GLUE(slotTwGfxDrawLineSegments, Err (*)(TwGfxSurfaceType*, const TwGfxPointType*, Int32, UInt32))(d, p, n, c)
#define TwGfxDrawRect(d, r, c) GLUE(slotTwGfxDrawRect, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*, UInt32))(d, r, c)
#define TwGfxFillRect(d, r, c) GLUE(slotTwGfxFillRect, Err (*)(TwGfxSurfaceType*, const TwGfxRectType*, UInt32))(d, r, c)
#define TwGfxDrawSpans(d, s, n, c) GLUE(slotTwGfxDrawSpans, Err (*)(TwGfxSurfaceType*, const TwGfxSpanType*, Int32, UInt32))(d, s, n, c)
#define TwGfxDrawBitmap(d, p, b) GLUE(slotTwGfxDrawBitmap, Err (*)(TwGfxSurfaceType*, const TwGfxPointType*, const TwGfxBitmapType*))(d, p, b)
/* ASSUMPTION: (surface, point, Palm BitmapType), from the game logs (r1 a pointer, r2 a bitmap resource). */
#define TwGfxDrawPalmBitmap(d, p, b) GLUE(slotTwGfxDrawPalmBitmap, Err (*)(TwGfxSurfaceType*, const TwGfxPointType*, const void*))(d, p, b)

#endif
