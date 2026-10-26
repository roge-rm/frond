/* Freestanding position-independent ARM/Thumb payload. Shared data is big endian.
 * Compile this twice; compatibility-thumb.S supplies the Thumb entry veneer. */
#include <PceNativeCall.h>
typedef unsigned long U32;
typedef unsigned char U8;
static U32 read32(const U8* p) {
    return ((U32)p[0] << 24) | ((U32)p[1] << 16) | ((U32)p[2] << 8) | p[3];
}
static void write32(U8* p, U32 v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
U32 compatibilityNative(const void* state, void* data, Call68KFuncType* call68k) {
    U8* p = data;
    U8 args[8];
    U32 value = read32(p), count = read32(p + 4), i;
    for (i = 0; i < count; ++i) value = (value * 1664525UL + 1013904223UL) ^ (value >> 7);
    write32(p + 8, value);
    write32(args, read32(p + 24));
    /* StrLen returns D0; the 68k function callback takes a long and a word. */
    write32(p + 12, call68k(state, 0x0c7, args, 4));
    write32(args, 0x2468ace0UL); args[4] = 0x12; args[5] = 0x34;
    write32(p + 16, call68k(state, read32(p + 20), args, 6));
    return value ^ 0xa55a5aa5UL;
}
