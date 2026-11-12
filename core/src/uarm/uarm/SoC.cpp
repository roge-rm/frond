#include <chrono>
#include "SoC.h"

#include <cstring>

#include "CPU.h"
#include "RAM.h"
#include "ROM.h"
#include "audio_queue.h"
#include "mem.h"
#include "nand.h"
#include "pace_patch.h"
#include "patch_dispatch.h"
#include "sdcard.h"
#include "system_state.h"

#define EVENT_QUEUE_CAPACITY 64

using namespace std;

namespace {
    uint64_t hostMsecNow() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

SoC::PenEvent SoC::PenEvent::PenDown(int x, int y) { return {.penDown = true, .x = x, .y = y, .hostMsec = hostMsecNow()}; }
SoC::PenEvent SoC::PenEvent::PenUp() { return {.penDown = false, .x = -1, .y = -1, .hostMsec = hostMsecNow()}; }

SoC::KeyEvent SoC::KeyEvent::KeyDown(enum KeyId key) { return {.keyDown = true, .key = key, .hostMsec = hostMsecNow()}; }
SoC::KeyEvent SoC::KeyEvent::KeyUp(enum KeyId key) { return {.keyDown = false, .key = key, .hostMsec = hostMsecNow()}; }

SoC::SoC()
    : savestate(make_unique<Savestate<ChunkType>>()),
      powerOnState(make_unique<Savestate<ChunkType>>()),
      penEventQueue(make_unique<Queue<PenEvent>>(EVENT_QUEUE_CAPACITY)),
      keyEventQueue(make_unique<Queue<KeyEvent>>(EVENT_QUEUE_CAPACITY)),
      audioQueue(nullptr) {
    pacePatch = createPacePatch();
    mem = memInit();

    patchDispatch = initPatchDispatch();
    systemState = createSystemState();
}

void SoC::KeyDown(enum KeyId key) {
    if (key != keyIdPower || PowerButtonSupported()) keyEventQueue->Push(KeyEvent::KeyDown(key));
}

void SoC::KeyUp(enum KeyId key) {
    if (key != keyIdPower || PowerButtonSupported()) keyEventQueue->Push(KeyEvent::KeyUp(key));
}

void SoC::PenDown(int x, int y) {
    penEventQueue->Push(PenEvent::PenDown(
        std::min<int>(std::max<int>(x, 0), displayConfiguration.width),
        std::min<int>(std::max<int>(y, 0),
                      displayConfiguration.height + displayConfiguration.graffitiHeight)));
}

void SoC::PenUp() { penEventQueue->Push(PenEvent::PenUp()); }

void SoC::SetFramebufferDirty(uint32_t pa, uint32_t size) {
    const uint32_t end = pa + size - 1;
    if (pa < framebufferAccessLowWatermark) framebufferAccessLowWatermark = pa;
    if (end > framebufferAccessHighWatermark) framebufferAccessHighWatermark = end;

    if (framebufferDirty) return;

    framebufferDirty = true;
    OnSetFramebufferDirty();
}

bool SoC::SetFramebuffer(uint32_t start, uint32_t size) {
    const bool tracked = SetFramebufferRange(start, size);
    WriteProtectFramebuffer();
    return tracked;
}

bool SoC::SetFramebufferRange(uint32_t start, uint32_t size) {
    const bool inSram = sramFb && size && start >= sramBase && start - sramBase + size <= sramSize;
    if (sramFb) {
        if (inSram) ramSetFramebuffer(sramFb, start, size);
        else ramSetNoFramebuffer(sramFb);
    }
    if (inSram) {
        ramSetNoFramebuffer(ram);
        return true;
    }
    if (size && (start < ramBase || start - ramBase + size > ramSize)) {
        fprintf(stderr, "framebuffer not in RAM\n");
        size = 0;
    }

    ramSetFramebuffer(ram, start, size);

    return size != 0;
}

void SoC::ClearFramebufferDirty() {
    framebufferAccessLowWatermark = ~0u;
    framebufferAccessHighWatermark = 0;

    framebufferDirty = false;
    WriteProtectFramebuffer();
}

void SoC::WriteProtectFramebuffer() {
    if (!cpu) return;
    for (ArmRam *r : {ram, sramFb}) {
        uint8_t *host;
        uint32_t size;
        if (r && ramFramebufferHost(r, &host, &size)) cpuWriteProtectHost(cpu, host, size);
    }
}

void SoC::Sleep() {
    if (sleeping || !OnSleep()) return;

    sleeping = true;
    cpuSetSleeping(cpu);

    // sleepAtTime = scheduler->GetTime();
    // printf("sleep\n");
}

void SoC::Wakeup(uint8_t wakeupSource) {
    if (!sleeping) return;
    sleeping = false;

    cpuWakeup(cpu);
    OnWakeup();

    // printf("wakeupt after %llu nsec from %u\n", scheduler->GetTime() - sleepAtTime,
    //        (int)wakeupSource);
}

void SoC::SetAudioQueue(struct AudioQueue *audioQueue) {
    this->audioQueue = audioQueue;

    OnSetAudioQueue(audioQueue);
}

void SoC::SetPcmOutputEnabled(bool pcmOutputEnabled) {
    if (audioQueue && pcmOutputEnabled) audioQueueClear(audioQueue);
    if (pcmOutputEnabled == pcmEnabled) return;

    pcmEnabled = pcmOutputEnabled;

    OnSetPcmOutputEnabled();
}

void SoC::SetPcmSuspended(bool pcmSuspended) {
    if (this->pcmSuspended == pcmSuspended) return;

    this->pcmSuspended = pcmSuspended;

    OnSetPcmSuspended();
}

Buffer SoC::GetRomData() { return {.size = romGetSize(rom), .data = romGetData(rom)}; }

struct Buffer SoC::GetNandData() { return nand ? nandGetData(nand) : Buffer{0, nullptr}; }

struct Buffer SoC::GetNandDirtyPages() {
    return nand ? nandGetDirtyPages(nand) : Buffer{0, nullptr};
}

bool SoC::IsNandDirty() { return nand ? nandIsDirty(nand) : false; }

void SoC::SetNandDirty(bool isDirty) {
    if (nand) nandSetDirty(nand, isDirty);
}

Buffer SoC::GetMemoryData() { return {.size = bufferMemory.size, .data = bufferMemory.buffer}; }

Buffer SoC::GetMemoryDirtyPages() {
    return {.size = bufferMemory.dirtyPagesSize, .data = bufferMemory.dirtyPages};
}

struct Buffer SoC::GetSavestate() {
    return {.size = savestate->GetSize(), .data = savestate->GetBuffer()};
}

void SoC::SdInsert() {
    if (cardInserted || !sdCardInitialized()) return;
    cardInserted = true;

    OnSdInsert();
}

bool SoC::SdRemount() {
    if (!cardInserted) return false;

    if (!sdCardInitialized() || strncmp(cardId, sdCardGetId(), SD_CARD_ID_MAX_LEN) != 0) {
        OnSdEject();
        cardInserted = false;

        return false;
    } else {
        OnSdRemount();
    }

    return true;
}

void SoC::SdEject() {
    if (!cardInserted) return;
    cardInserted = false;

    OnSdEject();
}

bool SoC::IsPacePatched() { return pacePatch->enterPace; }

void SoC::HardReset() {
    // The Zire's DAL only checks the power key after it's released, so its hard reset is a
    // power-on reset: the small ROM marks the RAM store invalid and Palm OS boots clean.
    // The TX and E2 ask "Erase all data?" when power is released after the Palm Powered logo,
    // which shows about 7.5 s into the boot, so power is held for 9 s.
    if (GetDeviceType() == DeviceType5::deviceTypeZire72) RequestPowerOnReset();
    else JamKey(keyIdPower, 9000);
    Reset();
}

void SoC::Reset() {
    penEventQueue->Clear();
    keyEventQueue->Clear();
    penDown = false;
    if (audioQueue) audioQueueClear(audioQueue);
    framebufferAccessLowWatermark = 0;
    framebufferAccessHighWatermark = ~0u;
}
