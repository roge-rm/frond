 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Memory access functions
  *
  * Copyright 1996 Bernd Schmidt
  */

#include "EmMemory.h"
#include "cputiming.h"

#define do_get_mem_long		EmMemDoGet32
#define do_get_mem_word		EmMemDoGet16
#define do_get_mem_byte		EmMemDoGet8

#define do_put_mem_long		EmMemDoPut32
#define do_put_mem_word		EmMemDoPut16
#define do_put_mem_byte		EmMemDoPut8

/* CPU operand accesses add their bank's wait states to gUaeAccessWait and its bus tag to
 * gUaeBusCount (cputiming.h); a long is two bus cycles. SDRAM banks look up the page instead.
 * Instruction fetches (get_i* in newcpu.h) are counted from the timing table. */

STATIC_INLINE void uae_count_access(emuptr a, int words, int write, int byte) {
    const EmAddressBank* bank = &EmMemGetBank(a);
    gUaeBusCount += words * bank->busTag;
    if (__builtin_expect(bank->sdram, 0)) {
        gUaeAccessWait += EmSdramAccess(a, write, words);
        if (write) gUaeSdramWriteWords += words;
    } else
        gUaeAccessWait += byte ? bank->waitByte : words * bank->waitWord;
}

STATIC_INLINE uint32 uae_get_long(emuptr a) { uae_count_access(a, 2, 0, 0); return EmMemGet32(a); }
STATIC_INLINE uint16 uae_get_word(emuptr a) { uae_count_access(a, 1, 0, 0); return EmMemGet16(a); }
STATIC_INLINE uint8 uae_get_byte(emuptr a) { uae_count_access(a, 1, 0, 1); return EmMemGet8(a); }

STATIC_INLINE void uae_put_long(emuptr a, uint32 v) { uae_count_access(a, 2, 1, 0); EmMemPut32(a, v); }
STATIC_INLINE void uae_put_word(emuptr a, uint16 v) { uae_count_access(a, 1, 1, 0); EmMemPut16(a, v); }
STATIC_INLINE void uae_put_byte(emuptr a, uint8 v) { uae_count_access(a, 1, 1, 1); EmMemPut8(a, v); }

#define get_long			uae_get_long
#define get_word			uae_get_word
#define get_byte			uae_get_byte

#define put_long			uae_put_long
#define put_word			uae_put_word
#define put_byte			uae_put_byte

