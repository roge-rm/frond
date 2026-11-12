//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "sspdev_AD7873.h"
#include <string.h>
#include <stdlib.h>
#include "cputil.h"
#include "Tg50Chunks.h"
#include "Tg50Trace.h"
#include "savestate/savestateAll.h"



struct Ad7873 {

	struct PxaGpio *gpio;
	int8_t penIntGpio;

	uint16_t prevVal;

	bool penDown;
	int16_t penX, penY;

	uint16_t vBattAdcVal, vAuxAdcVal;
};



static uint_fast16_t ad7873prvSspClientProc(void* userData, uint_fast8_t nBits, uint_fast16_t got)
{
	struct Ad7873 *ad = (struct Ad7873*)userData;
	uint16_t ret = ad->prevVal;


	//sometimes frame gpio is gpio, sometimes frame. in this system we act like we're always selected
	if (nBits != 12) {
		TG50_TRACE( "expecting 12-bit format!\n");
		return 0;
	}

	switch (got) {

		case 0x93:	//X+
			ad->prevVal = ad->penX;
			break;

		case 0xA6:	//Vbatt
			ad->prevVal = ad->vBattAdcVal;
			break;

		case 0xB8:	//Z1
			ad->prevVal = ad->penDown ? 0x7ff : 0x0000;
			break;

		case 0xE6:	//AUX (PD0 = 0)	(not sure how these differ)
		case 0xE7:	//AUX (PD0 = 1)
			ad->prevVal = ad->vAuxAdcVal;
			break;

		case 0xD3:	//Y+
		case 0xD0:
			ad->prevVal = ad->penY;
			break;

		default:
			TG50_TRACE( "AD7873: not sure how to deal with command 0x%04x\n", (unsigned)got);
			ad->prevVal = 0;
			break;
	}

	return ret;
}

void ad7873Reset(Ad7873 *ad) {
    ad->prevVal = 0;
    ad->penDown = false;
    ad->penX = ad->penY = -1;
    // Reset clears conversions; battery and accessory voltages remain external inputs.
    ad7873Periodic(ad);
}

struct Ad7873* ad7873Init(struct PxaSsp* ssp, struct PxaGpio* gpio, int8_t penIntGpio /* negative for none */)
{
	struct Ad7873 *ad = (struct Ad7873*)malloc(sizeof(*ad));

	if (!ad)
		ERR("cannot alloc AD7873");

	memset(ad, 0, sizeof (*ad));

	ad->gpio = gpio;
	ad->penIntGpio = penIntGpio;
    ad7873Reset(ad);

	//connect to the SSP
	if (!pxaSspAddClient(ssp, ad7873prvSspClientProc, ad))
		ERR("cannot add AD7873 to SSP\n");

	return ad;
}

void ad7873Periodic(struct Ad7873 *ad)
{
	if (ad->penIntGpio >= 0)
		pxaGpioSetState(ad->gpio, ad->penIntGpio, !ad->penDown);
}

void ad7873PenInput(struct Ad7873 *ad, int16_t x, int16_t y)
{
	ad->penDown = x >= 0 && y >= 0;

	if (ad->penDown) {
		ad->penY = x;		//yes they are cross-wired in the TG50
		ad->penX = y;
	}
	else {
		ad->penX = -1;
		ad->penY = -1;
	}
}

void ad7873setVbatt(struct Ad7873 *ad, uint16_t mV)	//Vref is 2.5V
{
	ad->vBattAdcVal = (mV / 4) * 4096 / 2500;
}

void ad7873setVaux(struct Ad7873 *ad, uint16_t mV)
{
	ad->vAuxAdcVal = mV * 4096 / 2500;
}


void ad7873Destroy(Ad7873 *ad) { free(ad); }
std::function<void()> ad7873CaptureInput(Ad7873 *ad) {
    const auto down = ad->penDown;
    const auto x = ad->penX, y = ad->penY;
    return [ad, down, x, y] { ad->penDown = down; ad->penX = x; ad->penY = y; };
}

template <typename H> static void ad7873State(Ad7873 &state, H &h) {
    h.Do16(state.prevVal).DoBool(state.penDown).Do16(state.penX).Do16(state.penY).Do16(state.vBattAdcVal).Do16(state.vAuxAdcVal);
}
template <typename T> void ad7873Save(Ad7873 *ad, T &savestate) {
    auto *chunk = savestate.GetChunk(tg50TouchChunk, 0);
    if (!chunk) ERR("cannot allocate TG50 model state");
    SaveChunkHelper h(*chunk);
    ad7873State(*ad, h);
}
template <typename T> bool ad7873Load(Ad7873 *ad, T &loader, bool preserveInput) {
    auto *chunk = loader.GetChunkOrFail(tg50TouchChunk, 0, "TG50 ad7873");
    if (!chunk) return false;
    auto next = *ad;
    LoadChunkHelper h(*chunk);
    ad7873State(next, h);
    if (chunk->HasError()) { loader.NotifyError(); return false; }
    if (preserveInput) {
        next.penDown = ad->penDown; next.penX = ad->penX; next.penY = ad->penY;
    } else { next.penDown = false; next.penX = next.penY = -1; }
    *ad = next;
    // Board refreshes output pins after GPIO, IC and all models have loaded.
    return true;
}
template void ad7873Save<Savestate<ChunkType>>(Ad7873*, Savestate<ChunkType>&);
template void ad7873Save<SavestateProbe<ChunkType>>(Ad7873*, SavestateProbe<ChunkType>&);
template bool ad7873Load<SavestateLoader<ChunkType>>(Ad7873*, SavestateLoader<ChunkType>&, bool);

uint16_t ad7873getVaux(const Ad7873 *ad) { return ad->vAuxAdcVal; }
