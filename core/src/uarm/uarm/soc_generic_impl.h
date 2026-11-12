#ifndef _SOC_GENERIC_IMPL_H_
#define _SOC_GENERIC_IMPL_H_

// clang-format off
#include "soc_generic.h"
// clang-format on

#include "CPU.h"
#include "Defer.h"
#include "MMU.h"
#include "MPU.h"
#include "SoC.h"
#include "cputil.h"
#include "savestate/savestateAll.h"
#include "system_state.h"

template <class T>
SocGeneric<T>::SocGeneric() : scheduler(std::make_unique<Scheduler<T>>(static_cast<T&>(*this))) {}

template <class T>
uint64_t SocGeneric<T>::Run(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    paceBreakSyscall = 0;

    return RunUntil<0, false>(maxCycles, cyclesPerSecond);
}

template <class T>
bool SocGeneric<T>::RunToPaceSyscall(uint16_t syscall, uint64_t maxCycles,
                                     uint64_t cyclesPerSecond) {
    if (syscall == paceBreakSyscall) return true;

    auto cpu = this->cpu;

    cpuSetBreakPaceSyscall(cpu, syscall);
    const uint64_t cycles =
        RunUntil<SLOW_PATH_REASON_PACE_SYSCALL_BREAK, false>(maxCycles, cyclesPerSecond);
    cpuSetBreakPaceSyscall(cpu, 0);

    this->AccountInjectedCycles(cycles, cyclesPerSecond);

    if ((cpuGetSlowPathReason(cpu) & SLOW_PATH_REASON_PACE_SYSCALL_BREAK) == 0) return false;

    paceBreakSyscall = syscall;

    return true;
}

#ifndef __EMSCRIPTEN__
template <class T>
bool SocGeneric<T>::RunToNativePc(uint32_t first, uint32_t second, uint64_t maxCycles, uint64_t cyclesPerSecond) {
    if (!first || !maxCycles || cyclesPerSecond < 1000 || maxCycles > 50000000 ||
        !cpuSetNativePcBreak(this->cpu, first, second)) return false;
    paceBreakSyscall = 0;
    Defer clear([&] { cpuSetNativePcBreak(this->cpu, 0, 0); });
    const uint64_t cycles = RunUntil<SLOW_PATH_REASON_NATIVE_PC, false>(maxCycles, cyclesPerSecond);
    this->AccountInjectedCycles(cycles, cyclesPerSecond);
    return cpuGetSlowPathReason(this->cpu) & SLOW_PATH_REASON_NATIVE_PC;
}

#endif

template <class T>
bool SocGeneric<T>::ExecuteInjected(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    auto cpu = this->cpu;

    const uint64_t cycles =
        RunUntil<SLOW_PATH_REASON_INJECTED_CALL_DONE | SLOW_PATH_REASON_NATIVE_FAULT, true>(maxCycles, cyclesPerSecond);

    this->AccountInjectedCycles(cycles, cyclesPerSecond);

    return cpuGetSlowPathReason(cpu) & SLOW_PATH_REASON_INJECTED_CALL_DONE;
}

template <class T>
void SocGeneric<T>::Reset() {
    this->BeforeStateReplacement();
    this->StateReplaced();
    paceBreakSyscall = 0;
    SoC::Reset();

#ifndef __EMSCRIPTEN__
    const auto networkMode = std::string(networkSimulation.GetModeName());
#endif
    const bool keyboardAttached = KeyboardAttached();
    const bool pcmOutputEnabled = pcmEnabled;
    const bool cardInserted = this->cardInserted;
    const uint64_t accumulatedTime = scheduler->GetTime();

    pcmSuspended = false;

    static_cast<T&>(*this).OnBeforeReset();
    SavestateLoader<ChunkType> loader;
    if (!loader.Load(powerOnState->GetBuffer(), powerOnState->GetSize(), static_cast<T&>(*this)))
        ERR("failed to restore power-on state");

#ifndef __EMSCRIPTEN__
    networkSimulation.SetMode(networkMode);
    networkSimulation.Reset();
#endif
    AttachKeyboard(keyboardAttached);
    if (cardInserted) SdInsert();

    SetPcmOutputEnabled(pcmOutputEnabled);

    if (jammedKey != keyInvalid && releaseJammedKeyAt > accumulatedTime && jammedKeyAt == accumulatedTime) {
        JamKey(jammedKey, (releaseJammedKeyAt - accumulatedTime) / 1_msec);
    } else if (jammedKey != keyInvalid) {
        // A reset during a hold (a hard reset's power) lets the key go.
        static_cast<T&>(*this).OnEngageKey(jammedKey, false);
        jammedKey = keyInvalid;
    }

    static_cast<T&>(*this).OnReset();
}

template <class T>
void SocGeneric<T>::DumpMMU() {
    if constexpr (T::MEMORY_SYSTEM_KIND == ARM_MEMORY_SYSTEM_MMU) {
        mmuDump(cpuGetMMU(cpu));
    } else {
        mpuDump(cpuGetMPU(cpu));
    }
}

template <class T>
bool SocGeneric<T>::Save() {
    Defer discard([&] { this->DiscardMemoryStickSaveImage(); });
    if (GetDeviceType()==deviceTypeTG50) savestate->Reset();
    return savestate->Save(*this);
}

template <class T>
bool SocGeneric<T>::Load(size_t savestateSize, void* savestateData) {
    if (savestateSize == 0 || !savestateData) return true;
    // Host input sits outside the savestate; a refused load puts it back too.
    SoC::InputCheckpoint input(*this);

    Savestate<ChunkType> savestate;
    SavestateLoader<ChunkType> loader;

    // A failed probe never reaches the real controller Save callback.
    {
        Defer discard([&] { this->DiscardMemoryStickSaveImage(); });
        if (!savestate.Save(*this)) return false;
    }
    if (!this->RestoringCheckpoint()) {
        jammedKey = keyInvalid;
        releaseJammedKeyAt = 0;
    }
    this->BeforeStateReplacement();

    if (!loader.Load(savestateData, savestateSize, *this)) {
        // Restore as a checkpoint, so scheduler phases, the keypad, queued host input
        // and attached serial clients are left as they were before this call.
        bool restored = false;
        this->StateReplacementRefused(
            [&] { restored = input.Restore(savestate.GetSize(), savestate.GetBuffer()); });
        if (!restored) ERR("failed to restore initial state after load failure");

        return false;
    }

    this->StateReplaced();
    return true;
}

template <class T>
void SocGeneric<T>::Load(SavestateLoader<ChunkType>& loader) {
    // The cached injection boundary belongs to the previous CPU execution state.
    paceBreakSyscall = 0;
    cpuLoad(cpu, loader);
    scheduler->Load(loader);
    systemStateLoad(systemState, loader);
#ifndef __EMSCRIPTEN__
    networkSimulation.Load(loader);
#endif

    uint32_t version;
    Chunk* chunk = loader.GetChunkOrFail(ChunkType::socGeneric, SOC_GENERIC_SAVESTATE_VERSION,
                                         "soc_generic", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    DoSaveLoad(helper, version);

    static_cast<T*>(this)->OnLoad(loader);
}

template <class T>
template <typename U>
void SocGeneric<T>::Save(U& savestate) {
    cpuSave(cpu, savestate);
    scheduler->Save(savestate);
    systemStateSave(systemState, savestate);
#ifndef __EMSCRIPTEN__
    networkSimulation.Save(savestate);
#endif

    auto* chunk = savestate.GetChunk(ChunkType::socGeneric, SOC_GENERIC_SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    strncpy(cardId, sdCardGetId(), sizeof(cardId));
    cardId[sizeof(cardId) - 1] = '\0';

    SaveChunkHelper helper(*chunk);
    DoSaveLoad(helper, SOC_GENERIC_SAVESTATE_VERSION);

    static_cast<T*>(this)->OnSave(savestate);
}

template <class T>
template <typename U>
void SocGeneric<T>::DoSaveLoad(U& chunkHelper, uint32_t version) {
    bool pcmEnabledDummy = pcmEnabled;

    chunkHelper.Do(typename U::BoolPack() << cardInserted << pcmEnabledDummy << sleeping)
        .DoBuffer(cardId, sizeof(cardId));

    uint64_t cyclesTotal = 0;
    if (version == 1) chunkHelper.Do64(cyclesTotal);
}

template <class T>
template <unsigned int breakReason, bool injected>
uint64_t SocGeneric<T>::RunUntil(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    if (!cyclesPerSecond) return 0;
    auto cpu = this->cpu;
    Scheduler<T>* scheduler = this->scheduler.get();

    const uint64_t previousBudget = runningCyclesPerSecond;
    runningCyclesPerSecond = cyclesPerSecond;
    Defer restoreBudget([&] { runningCyclesPerSecond = previousBudget; });
    uint64_t cycles = 0;

    while (cycles < maxCycles) {
        uint64_t cyclesToAdvance = scheduler->CyclesToNextUpdate(cyclesPerSecond);

        if (cyclesToAdvance > maxCycles - cycles) cyclesToAdvance = maxCycles - cycles;

        uint64_t cyclesAdvanced;

        if constexpr (injected) {
            cyclesAdvanced = cpuCycle<T::MEMORY_SYSTEM_KIND, injected>(cpu, cyclesToAdvance);
        } else {
            cyclesAdvanced = sleeping
                                 ? cyclesToAdvance
                                 : cpuCycle<T::MEMORY_SYSTEM_KIND, injected>(cpu, cyclesToAdvance);
        }

        scheduler->Advance(cyclesAdvanced, cyclesPerSecond);
        cycles += cyclesAdvanced;

        if constexpr (breakReason > 0) {
            if (cpuGetSlowPathReason(cpu) & breakReason) break;
        }
    }

    return cycles;
}

template <class T>
void SocGeneric<T>::PumpEventQueues() {
    eventQueueTicks++;

    PumpPenEventQueue();
    PumpKeyEventQueue();
}

template <class T>
void SocGeneric<T>::PumpPenEventQueue() {
    if (penEventQueue->GetSize() == 0) return;

    if (penDown && !penEventQueue->Peek().penDown &&
        eventQueueTicks - eventQueueTicksAtPenDown < MIN_EVENT_QUEUE_TICKS_BEFORE_PEN_UP) {
        return;
    }

    PenEvent evt(penEventQueue->Pop());

    if (evt.penDown) {
        while (penEventQueue->GetSize() > 0 && penEventQueue->Peek().penDown) {
            evt = penEventQueue->Pop();
        }
    }

    if (evt.penDown || penDown) {
        if (evt.penDown && !penDown) eventQueueTicksAtPenDown = eventQueueTicks;
        penDown = evt.penDown;

        static_cast<T&>(*this).OnTouch(evt.x, evt.y);
    }
}

template <class T>
void SocGeneric<T>::PumpKeyEventQueue() {
    if (jammedKey != keyInvalid && scheduler->GetTime() >= releaseJammedKeyAt) {
        static_cast<T&>(*this).OnEngageKey(jammedKey, false);
        jammedKey = keyInvalid;
    }

    if (keyEventQueue->GetSize() > 0) {
        if (HoldKeyEvent(keyEventQueue->Peek())) return;
        KeyEvent evt(keyEventQueue->Pop());
        if (evt.key == jammedKey) return;

        static_cast<T&>(*this).OnEngageKey(evt.key, evt.keyDown);
    }
}

template <class T>
uint64_t SocGeneric<T>::GetTime() {
    return scheduler->GetTime();
}

template <class T>
void SocGeneric<T>::JamKey(enum KeyId key, uint32_t durationMsec) {
    // A new hold lets go of the one it replaces, so no line stays pressed.
    if (jammedKey != keyInvalid && jammedKey != key) static_cast<T&>(*this).OnEngageKey(jammedKey, false);
    jammedKey = key;
    jammedKeyAt = scheduler->GetTime();
    releaseJammedKeyAt = jammedKeyAt + durationMsec * 1_msec;

    static_cast<T&>(*this).OnEngageKey(jammedKey, true);
}

#endif  // _SOC_GENERIC_IMPL_H_
