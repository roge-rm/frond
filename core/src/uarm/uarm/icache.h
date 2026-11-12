//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _ICACHE_H_
#define _ICACHE_H_

#include <cstdint>

#include "CPU.h"
#include "MMU.h"
#include "MPU.h"
#include "mem.h"

struct icache;

typedef uint32_t (*DecodeFn)(uint32_t opcode);

struct icache* icacheInit(struct ArmMem* mem, struct ArmMmu* mmu);
struct icache* icacheInit(struct ArmMem* mem, struct ArmMpu* mpu);

// XScale has two 32-byte fetch buffers even when instruction caching is disabled.
constexpr unsigned ICACHE_FETCH_STATE_WORDS = 19;
void icacheConfigureArm920(struct icache* ic);
void icacheArm920Control(struct icache* ic, bool enabled, bool roundRobin);
// Fast: fetches go to word first and the model keeps no lines; null for Exact.
void icacheArm920SetLeanWord(struct icache* ic, bool (*word)(void*, uint32_t, unsigned, bool, uint32_t*), void* ctx);
// Drops every line of the decode cache.
void icacheArm920DropDecoded(struct icache* ic);
bool icacheArm920Fetch(struct icache*, uint32_t va, unsigned size, bool privileged,
                       uint_fast8_t* fsr, uint32_t* instruction);
bool icacheArm920Word(struct icache* ic, uint32_t va, bool privileged,
                      uint_fast8_t* fsr, uint32_t* word);
// DDI0151C 3.9: two prefetched ARM instructions can retain the old MMU mapping.
void icacheArm920PrepareMmuSwitch(struct icache*, uint32_t nextPc, bool privileged);
bool icacheArm920ContinuationPending(const struct icache*);
void icacheArm920Branch(struct icache*);
// The ARM920 lines, replacement state and MMU-switch continuation, set without the side
// effects of a control write or an invalidation.
struct Arm920SavedState;
void icacheArm920Capture(const struct icache*, Arm920SavedState&);
void icacheArm920Apply(struct icache*, const Arm920SavedState&);
void icacheSetXscale(struct icache* ic, bool xscale);
void icacheGetFetchState(struct icache* ic, uint32_t* state);
bool icacheSetFetchState(struct icache* ic, const uint32_t* state);
void icacheInval(struct icache* ic);
// The guest's own whole-cache invalidation (MCR p15 c7). Same as icacheInval, but tells
// the JIT hook it came from the guest.
void icacheInvalGuest(struct icache* ic);
#ifdef FROND_DYNARMIC
// hook(ctx, va, size, guest): size 0 is the whole cache. Line invalidations always come as
// guest ones.
void icacheSetInvalHook(struct icache* ic, void (*hook)(void*, uint32_t, uint32_t, bool), void* ctx);
// The word at va (aligned) if an XScale fetch buffer holds its line, which icacheFetch
// uses before memory.
bool icacheBufferedWord(const struct icache* ic, uint32_t va, uint32_t* word);
// Fast only. watch(ctx, pa) flags the RAM line at pa so a store to it comes back through
// icacheDropFetchedHost, and returns its host address (null if it isn't RAM).
void icacheSetFetchWatch(struct icache* ic, const uint8_t* (*watch)(void*, uint32_t), void* ctx);
// Drops the fetch buffers holding any byte of host [host, host + size), so the next fetch
// reads the stored word. Exact keeps them, as an XScale runs the old word until invalidated.
void icacheDropFetchedHost(struct icache* ic, const uint8_t* host, uint32_t size);
// Watches every fetch buffer's line again, after the flags were cleared or a load.
void icacheWatchFetchBuffers(struct icache* ic);
bool icacheFetchUnwatched(const struct icache* ic);
#endif
void icacheInvalAddr(struct icache* ic, uint32_t addr);
void icacheInvalRange(struct icache* ic, uint32_t addr, uint32_t size);

// privileged is the mode the fetch is checked for (cpu->privileged).
template <int msys, int sz, int tier = 0>
bool icacheFetch(struct icache* ic, uint32_t va, bool privileged, uint_fast8_t* fsr,
                 uint32_t& instr, uint32_t& decoded);

#endif
