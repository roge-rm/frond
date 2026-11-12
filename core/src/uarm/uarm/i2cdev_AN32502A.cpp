//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "i2cdev_AN32502A.h"
#include <string.h>
#include <stdlib.h>
#include "cputil.h"
#include "Tg50Chunks.h"
#include "Tg50Trace.h"
#include "savestate/savestateAll.h"


struct An32502A {

	uint8_t inTransaction;
	uint8_t addrSeen;
	uint8_t ourAddr;
	uint8_t isRead;
	uint8_t regAddrSeen;
	uint8_t reg;
};

static bool an32502aPrvDoRegWrite(struct An32502A *an, uint8_t reg, uint8_t val)
{
	TG50_TRACE( "AN: writing 0x%02x -> 0x%02x\n", val, reg);

	if (reg > 2)
		return false;

	//value not needed for emulated system

	return true;
}

static uint8_t an32502aPrvDoRegRead(struct An32502A *an, uint8_t reg)
{
	TG50_TRACE( "AN32502A does not support register reading\n");

	return 0;
}

static uint_fast8_t an32502aPrvI2cHandler(void *userData, enum ActionI2C stimulus, uint_fast8_t value)
{
	struct An32502A *an = (struct An32502A*)userData;

	switch (stimulus) {
		case i2cStart:
			an->inTransaction = 1;
			an->regAddrSeen = 0;
			//fallthrough

		case i2cRestart:
			an->addrSeen = 0;
			an->ourAddr = 0;
			return 0;

		case i2cStop:
			an->inTransaction = 0;
			return 0;

		case i2cTx:
			if (!an->inTransaction)
				return 0;
			if (!an->addrSeen) {
				an->addrSeen = 1;
				if ((value >> 1) != 0x73)
					return 0;
				an->ourAddr = 1;
				an->isRead = value & 1;
				return 1;
			}
			if (!an->ourAddr)
				return 0;
			if (an->isRead) {

				TG50_TRACE( "unexpected write in read mode\n");
				return 0;
			}
			//write to us
			if (!an->regAddrSeen) {
				an->regAddrSeen = 1;
				an->reg = value;

				return 1;
			}
			//write to a reg
			return an32502aPrvDoRegWrite(an, an->reg++, value) ? 1 : 0;

		case i2cRx:
			if (!an->inTransaction || !an->addrSeen || !an->ourAddr)
				return 0;
			if (!an->isRead) {

				TG50_TRACE( "unexpected read in write mode\n");
				return 0;
			}
			if (!an->regAddrSeen) {

				TG50_TRACE( "unexpected read before register specified\n");
				return 0;
			}
			return an32502aPrvDoRegRead(an, an->reg++);

		default:
			return 0;
	}
}

struct An32502A* an32502aInit(struct PxaI2c* i2c)
{
	struct An32502A *an = (struct An32502A*)malloc(sizeof(*an));

	if (!an)
		ERR("cannot alloc AN32502A");

	memset(an, 0, sizeof (*an));

	if (!pxaI2cDeviceAdd(i2c, an32502aPrvI2cHandler, an))
		ERR("cannot add AN32502A to I2C\n");

	return an;
}


void an32502aReset(An32502A *an) { *an = {}; }
void an32502aDestroy(An32502A *an) { free(an); }

template <typename H> static void an32502aState(An32502A &state, H &h) {
    h.Do8(state.inTransaction).Do8(state.addrSeen).Do8(state.ourAddr).Do8(state.isRead).Do8(state.regAddrSeen).Do8(state.reg);
}
template <typename T> void an32502aSave(An32502A *an, T &savestate) {
    auto *chunk = savestate.GetChunk(tg50PowerChunk, 0);
    if (!chunk) ERR("cannot allocate TG50 model state");
    SaveChunkHelper h(*chunk);
    an32502aState(*an, h);
}
template <typename T> bool an32502aLoad(An32502A *an, T &loader, bool preserveInput) {
    auto *chunk = loader.GetChunkOrFail(tg50PowerChunk, 0, "TG50 an32502a");
    if (!chunk) return false;
    auto next = *an;
    LoadChunkHelper h(*chunk);
    an32502aState(next, h);
    if (chunk->HasError()) { loader.NotifyError(); return false; }
    if (next.inTransaction > 1 || next.addrSeen > 1 || next.ourAddr > 1 || next.isRead > 1 || next.regAddrSeen > 1) { loader.NotifyError(); return false; }
    *an = next;
    // Board refreshes output pins after GPIO, IC and all models have loaded.
    return true;
}
template void an32502aSave<Savestate<ChunkType>>(An32502A*, Savestate<ChunkType>&);
template void an32502aSave<SavestateProbe<ChunkType>>(An32502A*, SavestateProbe<ChunkType>&);
template bool an32502aLoad<SavestateLoader<ChunkType>>(An32502A*, SavestateLoader<ChunkType>&, bool);
