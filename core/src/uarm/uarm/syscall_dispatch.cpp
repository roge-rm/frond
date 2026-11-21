#include "syscall_dispatch.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <exception>
#include <memory>
#include "native_boundary.h"
#include "soc_IMX1.h"
#include "tg50_pace_session.h"
#include <map>

#include "CPU.h"
#include "Defer.h"
#include "SoC.h"
#include "cputil.h"
#include "memcpy.h"
#include "nand.h"
#include "pace.h"
#include "savestate/ChunkTypeUarm.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "syscall.h"
#include "syscall_68k.h"

#define MAX_NEST_LEVEL 4
#define INJECTED_CALL_IPS 100000000ull
#define INJECTED_CALL_TIMEOUT_SEC 4

#define PREPARE_INJECTED_CALL_IPS 50000000ull
#define PREPARE_INJECTED_CALL_TIMEOUT_SEC 1

#define INSTRUCTION_M68K_TRAP0 0x4E40

struct ScratchState {
    enum class Type { full, pace, minimal };

    void Load(SavestateLoader<ChunkType>& loader) {
        switch (type) {
            case Type::full:
                cpuFinishInjectedCall(cpu, loader);
                break;

            case Type::pace:
                paceLoad(loader);
                [[fallthrough]];

            case Type::minimal:
                cpuSetReg(cpu, 14, lr);
                cpuSetReg(cpu, 15, pc);
                cpuClearSlowPath(cpu, SLOW_PATH_REASON_INJECTED_CALL_DONE);
                cpuSetSlowPath(cpu, slowPathInjectedCallDone);
                cpuSetModePace(cpu, modePace);
                cpuSetCurInstrPC(cpu, curInstrPC);
                break;
        }
    }

    template <typename T>
    void Save(T& savestate) {
        cpuPrepareInjectedCall(cpu, savestate);
    }

    void Save(Type type) {
        this->type = type;

        savestate.Save(*this);

        if (type == Type::pace || type == Type::minimal) {
            const uint32_t* registers = cpuGetRegisters(cpu);
            lr = registers[14];
            pc = registers[15] + (cpuIsThumb(cpu) ? 1 : 0);

            slowPathInjectedCallDone =
                cpuGetSlowPathReason(cpu) & SLOW_PATH_REASON_INJECTED_CALL_DONE;

            modePace = cpuIsModePace(cpu);
            curInstrPC = cpuGetCurInstrPC(cpu);
        }
    }

    void Restore() { loader.Load(savestate.GetBuffer(), savestate.GetSize(), *this); }

    Type type{Type::full};
    uint32_t lr, pc;
    uint32_t curInstrPC;
    uint32_t slowPathInjectedCallDone;
    bool modePace;

    Savestate<ChunkType> savestate;
    SavestateLoader<ChunkType> loader;
    ArmCpu* cpu;
};

// A Zodiac guest call's frame: the machine's registers on the PACE site it starts from, the 68k state there and
// the host-side 68k registers' owner before it (pace.h external memory).
struct ZodiacFrame {
    uint32_t registers[16], cpsr, state, paceStatePtr;
    bool paceLive;
    uint8_t m68k[0x4c];
};

static bool zodiacPaceAccess(void* context, uint32_t address, void* data, uint32_t size, bool write) {
    auto* soc = static_cast<SocIMX1*>(context);
    return write ? soc->WriteGuest(address, data, size) : soc->ReadGuest(address, data, size);
}

struct SyscallDispatch {
    class SoC* soc;

    ScratchState scratchStates[MAX_NEST_LEVEL];
    size_t nestLevel{0};
    bool deadMansSwitch{false};
    bool recoverable{false};
    bool failed{false};
    std::unique_ptr<Tg50Pace::Session> tg50;
    std::exception_ptr tg50CallbackError;
    // The Zodiac runs its ROM PACE in the i.MX1 machine: calls go through SocIMX1 and the 68k helpers in
    // pace.h read its memory. paceLive: those helpers' registers are the current call's or callback's.
    SocIMX1* zodiac{nullptr};
    ZodiacFrame zodiacFrames[MAX_NEST_LEVEL];
    bool zodiacPaceLive{false};
    std::map<uint32_t, M68kTrapHandler> zodiacStubs;
};

// The pace.h helpers go to the Zodiac's memory only while its guest work needs them: a recovery scope, a call
// or a callback registration. Everywhere else they stay on the PXA's own memory, so a PACE exception or a PXA
// guest call never reaches the Zodiac, and a Zodiac that goes away leaves nothing behind.
static void zodiacPaceOn(struct SyscallDispatch* sd) { paceSetExternalMemory(zodiacPaceAccess, sd->zodiac); }
static void zodiacPaceOffWhenIdle(struct SyscallDispatch* sd) {
    if (sd->recoverable || sd->nestLevel) return;
    sd->zodiacPaceLive = false;
    paceReleaseExternalMemory(sd->zodiac);
}

struct SyscallDispatch* initSyscallDispatch(SoC* soc) {
    struct SyscallDispatch* sd = new SyscallDispatch();

    sd->soc = soc;
    for (auto& scratchState : sd->scratchStates) scratchState.cpu = soc->GetCpu();
    // A PXA SoC calls this mid-construction, before its device is set, so its type can't be asked for.
    sd->zodiac = dynamic_cast<SocIMX1*>(soc);

    return sd;
}

void syscallDispatchBeginRecovery(struct SyscallDispatch* sd) {
    if (sd->nestLevel || sd->recoverable) ERR("nested injected-call recovery scope");
    if (sd->zodiac) zodiacPaceOn(sd);
    if (sd->soc->GetDeviceType() == DeviceType5::deviceTypeTG50)
        sd->tg50 = std::make_unique<Tg50Pace::Session>(*sd->soc);
    sd->recoverable = true;
    sd->failed = false;
}
void syscallDispatchBeginFileTransfer(struct SyscallDispatch* sd, uint32_t bytes) {
    if (!sd->recoverable || !sd->tg50) throw std::runtime_error("File transfer needs an active TG50 transaction");
    sd->tg50->BeginFileTransfer(bytes);
}
void syscallDispatchFileProgress(struct SyscallDispatch* sd, uint32_t bytes) {
    if (!sd->recoverable || !sd->tg50) throw std::runtime_error("File progress needs an active TG50 transaction");
    sd->tg50->FileProgress(bytes);
}
bool syscallDispatchRecoveryActive(struct SyscallDispatch* sd) { return sd->recoverable; }
void syscallDispatchValidateCommit(struct SyscallDispatch* sd) {
    if (sd->tg50) {
        if (sd->failed || sd->nestLevel || sd->tg50CallbackError)
            throw std::runtime_error("TG50 transaction is unfinished or failed");
        sd->tg50->Close();
    }
}
void syscallDispatchEndRecovery(struct SyscallDispatch* sd) {
    if (sd->tg50) sd->tg50->AbortHandlers();
    sd->tg50.reset();
    sd->tg50CallbackError = nullptr;
    if (sd->zodiac) {
        sd->zodiacStubs.clear();
        sd->zodiac->SetTrap0Hook(nullptr);
        sd->zodiacPaceLive = false;
        paceReleaseExternalMemory(sd->zodiac);
    } else
        cpuClearNativeRuntime(sd->soc->GetCpu());
    sd->recoverable = false;
    sd->failed = false;
    sd->nestLevel = 0;
    sd->deadMansSwitch = false;
}

static void injectedCallTimeout(struct SyscallDispatch* sd) {
    if (sd->recoverable || sd->zodiac) {
        sd->failed = true;
        throw InjectedCallError();
    }
    ERR("failed to execute injected call within time limit\n");
}

bool syscallDispatchInProgress(struct SyscallDispatch* sd) { return sd->nestLevel > 0; }

bool syscallDispatchM68kSupport(struct SyscallDispatch* sd) { return sd->soc->PrepareM68kSupport(); }

bool syscallDispatchPrepare(struct SyscallDispatch* sd, SyscallPrepareMode mode) {
    if (sd->zodiac) {
        if (sd->nestLevel || !syscallDispatchM68kSupport(sd)) return false;
        auto* soc = sd->zodiac;
        uint32_t registers[16];
        // Already stopped at the app's EvtGetEvent (a call before this one).
        bool ready = soc->GuestCallPending() && soc->GuestRegisters(registers) &&
                     registers[15] == soc->PaceSites().syscallSite &&
                     (registers[1] & 0xffff) == SYSCALL_68K_EVT_GET_EVENT;
        if (!ready)
            ready = soc->RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT,
                                          PREPARE_INJECTED_CALL_TIMEOUT_SEC * PREPARE_INJECTED_CALL_IPS,
                                          PREPARE_INJECTED_CALL_IPS);
        // A 68k app waiting for events with no timeout never gets back to EvtGetEvent by itself.
        if (!ready && mode == SYSCALL_PREPARE_WAKE && soc->WakeIdle())
            ready = soc->RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT,
                                          PREPARE_INJECTED_CALL_TIMEOUT_SEC * PREPARE_INJECTED_CALL_IPS,
                                          PREPARE_INJECTED_CALL_IPS);
        // Inside a recovery scope the helpers stay on the Zodiac; with none (a look that opens no
        // transaction) they go back, as nothing would turn them off later.
        if (ready && sd->recoverable) zodiacPaceOn(sd);
        else zodiacPaceOffWhenIdle(sd);
        return ready;
    }
    if (sd->soc->GetDeviceType() == DeviceType5::deviceTypeTG50) {
        try {
            if (!Tg50Pace::OriginalLayout(*sd->soc)) return false;
            if (sd->tg50) { sd->tg50->CheckOwner(); return true; }
            if (sd->nestLevel || !syscallDispatchM68kSupport(sd)) return false;
            if (!sd->soc->RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT,
                    PREPARE_INJECTED_CALL_TIMEOUT_SEC * PREPARE_INJECTED_CALL_IPS,
                    PREPARE_INJECTED_CALL_IPS)) return false;
            Tg50Pace::Boundary::Capture(*sd->soc, true);
            return true;
        } catch (const std::runtime_error&) { return false; }
    }
    if (!syscallDispatchM68kSupport(sd)) return false;
    if (sd->nestLevel > 0) ERR("syscallDispatchPrepare is only allowed on top level\n");

    const bool after = mode == SYSCALL_PREPARE_AFTER_NATIVE;
    if (sd->soc->RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT,
                                  PREPARE_INJECTED_CALL_TIMEOUT_SEC * PREPARE_INJECTED_CALL_IPS / (after ? 4 : 1),
                                  PREPARE_INJECTED_CALL_IPS))
        return true;
    // A 68k app waiting for events with no timeout never gets back to EvtGetEvent by itself.
    return mode == SYSCALL_PREPARE_WAKE && NativeBoundary::WakeIdle(*sd->soc) &&
           sd->soc->RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT,
                                     PREPARE_INJECTED_CALL_TIMEOUT_SEC * PREPARE_INJECTED_CALL_IPS,
                                     PREPARE_INJECTED_CALL_IPS);
}

void syscallDispatchValidateMemory(struct SyscallDispatch* sd, uint32_t address, size_t size, bool write) {
    if (sd->soc->GetDeviceType() == DeviceType5::deviceTypeTG50) {
        if (!sd->tg50 || !sd->recoverable || size > UINT32_MAX) {
            if (sd->tg50) sd->tg50->Fail();
            sd->failed = true;
            throw std::runtime_error("TG50 transfer needs an active guest transaction");
        }
        sd->tg50->BeforeTransfer(address, uint32_t(size), write);
    }
}

bool syscallDispatch_strncpy_toHost(struct SyscallDispatch* sd, void* dest, uint32_t src,
                                    size_t maxLen) {
    struct ArmCpu* cpu = sd->soc->GetCpu();
    syscallDispatchValidateMemory(sd, src, maxLen, false);
    memset(dest, 0, maxLen);
    if (sd->zodiac) {
        for (size_t i = 0; i < maxLen; i++) {
            if (!sd->zodiac->ReadGuest(src + i, static_cast<char*>(dest) + i, 1)) return false;
            if (static_cast<char*>(dest)[i] == '\0') break;
        }
        return true;
    }

    for (size_t i = 0; i < maxLen; i++) {
        if (!cpuMemOpExternal(cpu, dest, src, 1, false)) return false;
        if (*(const char*)(dest) == '\0') break;

        dest = (char*)dest + 1;
        src++;
    }

    return true;
}

bool syscallDispatch_memcpy_fromHost(struct SyscallDispatch* sd, uint32_t dest, const void* src,
                                     size_t size) {
    struct ArmCpu* cpu = sd->soc->GetCpu();
    MemcpyResult memcpyResult;
    syscallDispatchValidateMemory(sd, dest, size);
    if (sd->zodiac) return size > UINT32_MAX ? false : sd->zodiac->WriteGuest(dest, src, uint32_t(size));
    if (sd->tg50) {
        if (!cpuNativeBuffer(cpu, dest, const_cast<void*>(src), size, true, false)) {
            sd->failed = true;
            sd->tg50->Fail();
            throw std::runtime_error("TG50 guest write failed");
        }
        return true;
    }
    memcpy_hostToArm(dest, reinterpret_cast<const uint8_t*>(src), size, true, cpu, &memcpyResult);

    return memcpyResult.ok;
}

bool syscallDispatch_memcpy_toHost(struct SyscallDispatch* sd, void* dest, uint32_t src,
                                   size_t size) {
    struct ArmCpu* cpu = sd->soc->GetCpu();
    MemcpyResult memcpyResult;
    syscallDispatchValidateMemory(sd, src, size, false);
    if (sd->zodiac) return size > UINT32_MAX ? false : sd->zodiac->ReadGuest(src, dest, uint32_t(size));
    if (sd->tg50) {
        if (!cpuNativeBuffer(cpu, src, dest, size, false, false)) {
            sd->failed = true;
            sd->tg50->Fail();
            throw std::runtime_error("TG50 guest read failed");
        }
        return true;
    }
    memcpy_armToHost(reinterpret_cast<uint8_t*>(dest), src, size, true, cpu, &memcpyResult);

    return memcpyResult.ok;
}

static size_t pushState(struct SyscallDispatch* sd, ScratchState::Type type) {
    if (sd->soc->GetDeviceType() == DeviceType5::deviceTypeTG50) {
        if (!sd->tg50 || !sd->recoverable)
            throw std::runtime_error("TG50 call needs an active guest transaction");
        sd->tg50->BeforeCall();
    }
    if (sd->nestLevel >= MAX_NEST_LEVEL) {
        if (sd->zodiac) throw std::runtime_error("Zodiac injected call nesting limit exceeded");
        if (sd->tg50) {
            sd->failed = true;
            sd->tg50->Fail();
            throw std::runtime_error("TG50 injected call nesting limit exceeded");
        }
        ERR("unable to dispatch syscall: max nest level reached\n");
    }

    if (sd->zodiac) {
        // From the syscall site, or the TRAP #0 site in a callback: r0 is the 68k state on both.
        auto& frame = sd->zodiacFrames[sd->nestLevel];
        auto* soc = sd->zodiac;
        zodiacPaceOn(sd);
        if (sd->zodiacPaceLive && !paceSave68kState()) throw std::runtime_error("Zodiac: could not write the 68k state");
        frame.paceLive = sd->zodiacPaceLive;
        frame.paceStatePtr = paceGetStatePtr();
        if (!soc->GuestCallPending() || !soc->GuestRegisters(frame.registers, &frame.cpsr) ||
            (frame.registers[15] != soc->PaceSites().syscallSite && frame.registers[15] != soc->PaceSites().trap0Site))
            throw std::runtime_error("Zodiac: a guest call needs the guest on a PACE site");
        frame.state = frame.registers[0];
        if (!soc->ReadGuest(frame.state, frame.m68k, sizeof(frame.m68k)))
            throw std::runtime_error("Zodiac: could not read the 68k state");
        paceSetStatePtr(frame.state);
        if (!paceLoad68kState()) throw std::runtime_error("Zodiac: could not read the 68k state");
        sd->zodiacPaceLive = true;
        return sd->nestLevel++;
    }
    const uint32_t nestLevel = sd->nestLevel++;
    sd->scratchStates[nestLevel].Save(type);

    return nestLevel;
}

static void popState(struct SyscallDispatch* sd, size_t nestLevel) {
    if (sd->nestLevel != nestLevel + 1) {
        if (sd->tg50) {
            sd->failed = true;
            sd->tg50->Fail();
            throw std::runtime_error("TG50 injected call nesting state changed");
        }
        ERR("bad nest level\n");
    }

    if (sd->zodiac) {
        // The 68k state and registers as they were, and the helpers back on the frame's owner.
        const auto& frame = sd->zodiacFrames[nestLevel];
        auto* soc = sd->zodiac;
        bool ok = soc->WriteGuest(frame.state, frame.m68k, sizeof(frame.m68k));
        for (unsigned r = 0; ok && r < 15; ++r) ok = soc->SetGuestRegister(r, frame.registers[r]);
        ok = ok && soc->SetGuestRegister(16, frame.cpsr) && soc->SetGuestRegister(15, frame.registers[15]);
        paceSetStatePtr(frame.paceStatePtr);
        sd->zodiacPaceLive = frame.paceLive;
        if (ok && frame.paceLive) ok = paceLoad68kState();
        sd->nestLevel--;
        zodiacPaceOffWhenIdle(sd);
        if (!ok) throw std::runtime_error("Zodiac: could not put the guest back after a call");
        return;
    }
    sd->scratchStates[nestLevel].Restore();
    sd->nestLevel--;
}

void syscallDispatchRegisterM68kStub(struct SyscallDispatch* sd, uint32_t trampoline,
                                     M68kStub stub, uint32_t argumentBytes) {
    if (sd->soc->GetDeviceType() == DeviceType5::deviceTypeTG50) {
        if (!sd->tg50 || !sd->recoverable)
            throw std::runtime_error("TG50 callback needs an active guest transaction");
        sd->tg50->Register(trampoline, argumentBytes);
    }
    if (sd->zodiac) {
        // The stub runs from the ROM PACE's TRAP #0 frame (SocIMX1::SetTrap0Hook), with the helpers on the
        // 68k state there. The guest goes on at an RTS in the ROM, past the bl, so the ROM PACE returns
        // itself: a native caller's return address is a magic one only its RTS knows.
        sd->zodiacStubs[trampoline] = [=](struct ArmCpu* cpu, uint32_t) -> bool {
            const uint32_t sp = paceGetAreg(7);
            stub(cpu, sp + 4, [=]() { sd->deadMansSwitch = true; });
            paceSetAreg(7, sp);
            paceSetPC(sd->zodiac->PaceSites().rts68k);
            return true;
        };
        zodiacPaceOn(sd);
        paceSet16(trampoline, INSTRUCTION_M68K_TRAP0);
        zodiacPaceOffWhenIdle(sd);
        sd->zodiac->SetTrap0Hook([sd]() -> bool {
            auto* soc = sd->zodiac;
            uint32_t registers[16], pc;
            if (!soc->GuestRegisters(registers) || !soc->ReadGuest(registers[0] + 0x44, &pc, 4)) return false;
            const auto stub = sd->zodiacStubs.find(pc - 2);
            uint8_t opcode[2];
            if (stub == sd->zodiacStubs.end() || !soc->ReadGuest(pc - 2, opcode, 2) ||
                (opcode[0] << 8 | opcode[1]) != INSTRUCTION_M68K_TRAP0) return false;
            const uint32_t ownerState = paceGetStatePtr();
            const bool ownerLive = sd->zodiacPaceLive;
            paceSetStatePtr(registers[0]);
            if (!paceLoad68kState()) throw std::runtime_error("Zodiac: could not read the callback's 68k state");
            sd->zodiacPaceLive = true;
            stub->second(nullptr, pc - 2);
            if (!paceSave68kState() || !soc->SkipTrap0Callout())
                throw std::runtime_error("Zodiac: could not return from a callback");
            paceSetStatePtr(ownerState);
            sd->zodiacPaceLive = ownerLive;
            return true;
        });
        return;
    }
    M68kTrapHandler trapHandler = [=](struct ArmCpu* cpu, uint32_t address) -> bool {
        if (sd->tg50) {
            try {
                const uint32_t sp = sd->tg50->BeforeCallback(address);
                stub(cpu, sp + 4, [=]() { sd->deadMansSwitch = true; });
                sd->tg50->AfterCallback(address, sp);
                paceSetAreg(7, sp + 4);
                paceSetPC(PACE_RETURN_MAGIC_PC);
            } catch (...) {
                sd->tg50CallbackError = std::current_exception();
                sd->failed = true;
                sd->tg50->Fail();
                cpuSetSlowPath(cpu, SLOW_PATH_REASON_NATIVE_FAULT);
            }
            return true;
        }
        const uint32_t sp = paceGetAreg(7);
        const uint32_t returnAddress = paceGet32(sp);
        stub(cpu, sp + 4, [=]() { sd->deadMansSwitch = true; });
        paceSetAreg(7, sp + 4);
        paceSetPC(returnAddress);
        return true;
    };
    if (sd->tg50) {
        const uint8_t instruction[]{0x4e, 0x40};
        syscallDispatch_memcpy_fromHost(sd, trampoline, instruction, sizeof(instruction));
    } else
        paceSet16(trampoline, INSTRUCTION_M68K_TRAP0);
    cpuAddM68kTrap0Handler(sd->soc->GetCpu(), trampoline, trapHandler);
}

void syscallDispatchUnregisterM68kStub(struct SyscallDispatch* sd, uint32_t trampoline) {
    if (sd->zodiac) {
        sd->zodiacStubs.erase(trampoline);
        if (sd->zodiacStubs.empty()) sd->zodiac->SetTrap0Hook(nullptr);
        return;
    }
    if (sd->tg50) {
        if (sd->failed || std::uncaught_exceptions()) sd->tg50->Fail();
        sd->tg50->Unregister(trampoline);
        return;
    }
    cpuRemoveM68kTrap0Handler(sd->soc->GetCpu(), trampoline);
}

static bool executeInjectedCall(struct SyscallDispatch* sd, uint32_t flags) {
    auto* soc = sd->soc;
    struct ArmCpu* cpu = soc->GetCpu();
    if (sd->tg50) {
        auto& budget = sd->tg50->ExecutionBudget();
        const uint64_t localStart = soc->GetInjectedTimeNsec();
        uint64_t pureCycles = 0;
        while (true) {
            const uint64_t slice = budget.Slice(soc->GetInjectedTimeNsec());
            bool complete = false;
            if (flags & SC_EXECUTE_FULL) {
                const uint64_t before = soc->GetInjectedTimeNsec();
                complete = soc->ExecuteInjected(slice, INJECTED_CALL_IPS);
                const uint64_t elapsed = soc->GetInjectedTimeNsec() - before;
                // At 100 MHz each completed cycle contributes exactly10 ns.
                // Nested execution is conservatively counted by both scopes.
                budget.Charge(elapsed / 10, soc->GetInjectedTimeNsec());
            } else {
                uint64_t cycles = 0;
                uint32_t instructions = 0;
                do {
                    cycles += cpuCyclePure(cpu);
                    if (cpuGetSlowPathReason(cpu) &
                        (SLOW_PATH_REASON_INJECTED_CALL_DONE | SLOW_PATH_REASON_NATIVE_FAULT)) break;
                    if (!(++instructions & 1023)) budget.Check(soc->GetInjectedTimeNsec());
                } while (cycles < slice);
                pureCycles += cycles;
                budget.Charge(cycles, soc->GetInjectedTimeNsec());
                complete = cpuGetSlowPathReason(cpu) & SLOW_PATH_REASON_INJECTED_CALL_DONE;
            }
            if (sd->tg50CallbackError) std::rethrow_exception(sd->tg50CallbackError);
            if (complete) return true;
            budget.Check(soc->GetInjectedTimeNsec());
            if (soc->GetInjectedTimeNsec() - localStart >= 4000000000ull ||
                pureCycles >= INJECTED_CALL_TIMEOUT_SEC * INJECTED_CALL_IPS) return false;
        }
    }

    if (flags & SC_EXECUTE_FULL) {
        if (soc->ExecuteInjected(INJECTED_CALL_TIMEOUT_SEC * INJECTED_CALL_IPS,
                                 INJECTED_CALL_IPS)) {
            return true;
        }
    } else {
        uint64_t cycles = 0;

        do {
            cycles += cpuCyclePure(cpu);
            if (cpuGetSlowPathReason(cpu) & SLOW_PATH_REASON_INJECTED_CALL_DONE) return true;
        } while (cycles < INJECTED_CALL_TIMEOUT_SEC * INJECTED_CALL_IPS);
    }

    return false;
}

static void rejectTg50NativeCall(struct SyscallDispatch* sd) {
    if (sd->zodiac) throw std::runtime_error("Zodiac native ARM calls aren't available");
    if (sd->soc->GetDeviceType() != DeviceType5::deviceTypeTG50) return;
    if (sd->tg50) sd->tg50->Fail();
    sd->failed = true;
    throw std::runtime_error("TG50 native ARM injection is unqualified");
}

static void executeInjectedSyscall(struct SyscallDispatch* sd, uint32_t flags, uint16_t syscall) {
    rejectTg50NativeCall(sd);
    cpuStartInjectedSyscall(sd->soc->GetCpu(), syscall);

    if (!executeInjectedCall(sd, flags)) {
        injectedCallTimeout(sd);
    }
}

static void executeInjectedSyscall68k(struct SyscallDispatch* sd, uint32_t flags,
                                      uint16_t syscall) {
    if (sd->zodiac) {
        // Through the ROM's dispatcher from the frame's site, with the arguments pushed on the 68k stack.
        auto* soc = sd->zodiac;
        const uint32_t state = sd->zodiacFrames[sd->nestLevel - 1].state;
        if (!paceSave68kState() || !soc->SetGuestRegister(0, state) || !soc->SetGuestRegister(1, syscall) ||
            !soc->SetGuestRegister(14, zodiacInjectedReturn) ||
            !soc->SetGuestRegister(15, soc->PaceSites().calloutSyscall))
            throw std::runtime_error("Zodiac: could not start a guest call");
        const bool deadMansSwitchSaved = sd->deadMansSwitch;
        while (true) {
            sd->deadMansSwitch = false;
            if (soc->ExecuteInjected(INJECTED_CALL_TIMEOUT_SEC * INJECTED_CALL_IPS, INJECTED_CALL_IPS)) break;
            if (sd->deadMansSwitch) continue;
            injectedCallTimeout(sd);
        }
        sd->deadMansSwitch = deadMansSwitchSaved;
        paceSetStatePtr(state);
        if (!paceLoad68kState()) throw std::runtime_error("Zodiac: could not read the 68k state after a call");
        return;
    }
    struct ArmCpu* cpu = sd->soc->GetCpu();
    struct NAND* nand = sd->soc->GetNand();
    bool deadMansSwitchSaved = sd->deadMansSwitch;

    cpuStartInjectedSyscall68k(cpu, syscall);

    while (true) {
        const uint32_t nandWriteCnt = nand ? nandGetWriteCnt(nand) : 0;
        sd->deadMansSwitch = false;

        if (executeInjectedCall(sd, flags)) break;
        if (sd->deadMansSwitch || (nand && nandGetWriteCnt(nand) != nandWriteCnt)) continue;

        injectedCallTimeout(sd);
    }

    cpuFinishInjectedSyscall68k(cpu);

    sd->deadMansSwitch = deadMansSwitchSaved;
}

static ScratchState::Type nativeCallPushType(uint32_t flags) {
    return (flags & SC_EXECUTE_FULL) ? ScratchState::Type::full : ScratchState::Type::minimal;
}

uint16_t syscall_SysSetAutoOffTime(struct SyscallDispatch* sd, uint32_t flags, uint32_t timeout) {
    rejectTg50NativeCall(sd);
    const size_t nestLevel = pushState(sd, nativeCallPushType(flags));
    uint32_t* registers = cpuGetRegisters(sd->soc->GetCpu());

    registers[0] = timeout;
    executeInjectedSyscall(sd, flags, SYSCALL_SYS_SET_AUTO_OFF_TIME);

    const uint16_t err = registers[0];
    popState(sd, nestLevel);

    return err;
}

uint16_t syscall_MemPtrNew(struct SyscallDispatch* sd, uint32_t flags, uint32_t size) {
    rejectTg50NativeCall(sd);
    const size_t nestLevel = pushState(sd, nativeCallPushType(flags));
    uint32_t* registers = cpuGetRegisters(sd->soc->GetCpu());

    registers[0] = size;
    executeInjectedSyscall(sd, flags, SYSCALL_MEM_PTR_NEW);

    const uint16_t err = registers[0];
    popState(sd, nestLevel);

    return err;
}

uint16_t syscall_MemPtrFree(struct SyscallDispatch* sd, uint32_t flags, uint32_t ptr) {
    rejectTg50NativeCall(sd);
    const size_t nestLevel = pushState(sd, nativeCallPushType(flags));
    uint32_t* registers = cpuGetRegisters(sd->soc->GetCpu());

    registers[0] = ptr;
    executeInjectedSyscall(sd, flags, SYSCALL_MEM_CHUNK_FREE);

    const uint16_t err = registers[0];
    popState(sd, nestLevel);

    return err;
}

uint16_t syscall_FtrGet(struct SyscallDispatch* sd, uint32_t flags, uint32_t creator,
                        uint16_t ftrNum, uint32_t valueP) {
    rejectTg50NativeCall(sd);
    const size_t nestLevel = pushState(sd, nativeCallPushType(flags));
    uint32_t* registers = cpuGetRegisters(sd->soc->GetCpu());

    registers[0] = creator;
    registers[1] = ftrNum;
    registers[2] = valueP;
    executeInjectedSyscall(sd, flags, SYSCALL_FTR_GET);

    const uint16_t err = registers[0];
    popState(sd, nestLevel);

    return err;
}

uint16_t syscall_FtrSet(struct SyscallDispatch* sd, uint32_t flags, uint32_t creator,
                        uint16_t ftrNum, uint32_t value) {
    rejectTg50NativeCall(sd);
    const size_t nestLevel = pushState(sd, nativeCallPushType(flags));
    uint32_t* registers = cpuGetRegisters(sd->soc->GetCpu());

    registers[0] = creator;
    registers[1] = ftrNum;
    registers[2] = value;
    executeInjectedSyscall(sd, flags, SYSCALL_FTR_SET);

    const uint16_t err = registers[0];
    popState(sd, nestLevel);

    return err;
}

static uint32_t syscall68k(struct SyscallDispatch* sd, uint32_t flags, uint16_t syscall,
                           bool returnPtr, std::function<void()> setupStack) {
    // Do not execute guest cleanup calls while unwinding a failed export.
    if (sd->failed) return 0;
    const size_t nestLevel = pushState(
        sd, (flags & SC_EXECUTE_FULL) ? ScratchState::Type::pace : ScratchState::Type::full);

    const bool suspendTimers = (flags & SC_EXECUTE_FULL) && !(flags & SC_EXECUTE_TIMERS);
    if (suspendTimers) sd->soc->SuspendTimerInterrupts(true);
    Defer resumeTimers([=]() {
        if (suspendTimers) sd->soc->SuspendTimerInterrupts(false);
    });

    try {
        setupStack();
        executeInjectedSyscall68k(sd, flags, syscall);
        const uint32_t result = returnPtr ? paceGetAreg(0) : paceGetDreg(0);
        popState(sd, nestLevel);
        if (sd->tg50) sd->tg50->AfterCall();
        return result;
    } catch (...) {
        sd->failed = true;
        if (sd->tg50) sd->tg50->Fail();
        throw;
    }
}

uint32_t syscall68k_SysGetOsVersionString(struct SyscallDispatch* sd, uint32_t flags) {
    return syscall68k(sd, flags, SYSCALL_68K_SYS_GET_OS_VERSION_STRING, true, []() {});
}

uint32_t syscall68k_MemPtrNew(struct SyscallDispatch* sd, uint32_t flags, uint32_t size) {
    const uint32_t result = syscall68k(sd, flags, SYSCALL_68K_MEM_PTR_NEW, true, [=]() { pacePush32(size); });
    if (sd->tg50) sd->tg50->AddAllocation(result, size);
    return result;
}

uint16_t syscall68k_MemPtrFree(struct SyscallDispatch* sd, uint32_t flags, uint32_t memPtr) {
    if (sd->failed) return 0;
    if (sd->tg50) {
        if (std::uncaught_exceptions()) {
            sd->failed = true;
            sd->tg50->Fail();
            return 0;
        }
        try {
            sd->tg50->BeforeFree(memPtr);
            const uint16_t result = syscall68k(sd, flags, SYSCALL_68K_MEM_CHUNK_FREE,
                                               false, [=]() { pacePush32(memPtr); });
            sd->tg50->AfterFree(memPtr, result);
            return result;
        } catch (...) {
            // Defer cleanup cannot throw. Close reports the latched failure before publication.
            sd->failed = true;
            sd->tg50->Fail();
            return 0;
        }
    }
    return syscall68k(sd, flags, SYSCALL_68K_MEM_CHUNK_FREE, false,
                       [=]() { pacePush32(memPtr); });
}

uint16_t syscall68k_SysSetAutoOffTime(struct SyscallDispatch* sd, uint32_t flags, uint16_t seconds) {
    return syscall68k(sd, flags, SYSCALL_68K_SYS_SET_AUTO_OFF_TIME, false,
                       [=]() { pacePush16(seconds); });
}

uint16_t syscall68k_MemNumCards(struct SyscallDispatch* sd, uint32_t flags) {
    return syscall68k(sd, flags, SYSCALL_68K_MEM_NUM_CARDS, false, []() {});
}

uint16_t syscall68k_ExgDBRead(struct SyscallDispatch* sd, uint32_t flags, uint32_t readProcP,
                              uint32_t deleteProcP, uint32_t userDataP, uint32_t dbIDP,
                              uint16_t cardNo, uint32_t needsResetP, bool keepDates) {
    return syscall68k(sd, flags, SYSCALL_68K_EXG_DB_READ, false, [=]() {
        pacePush8(keepDates);
        pacePush32(needsResetP);
        pacePush16(cardNo);
        pacePush32(dbIDP);
        pacePush32(userDataP);
        pacePush32(deleteProcP);
        pacePush32(readProcP);
    });
}

uint16_t syscall68k_ExgDBWrite(struct SyscallDispatch* sd, uint32_t flags, uint32_t writeProcP,
                               uint32_t userDataP, uint32_t nameP, uint32_t dbID, uint16_t cardNo) {
    return syscall68k(sd, flags, SYSCALL_68K_EXG_DB_WRITE, false, [=]() {
        pacePush16(cardNo);
        pacePush32(dbID);
        pacePush32(nameP);
        pacePush32(userDataP);
        pacePush32(writeProcP);
    });
}

uint16_t syscall68k_DmNumDatabases(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo) {
    return syscall68k(sd, flags, SYSCALL_68K_DM_NUM_DATABASES, false,
                      [=]() { pacePush16(cardNo); });
}

uint32_t syscall68k_DmGetDatabase(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                  uint16_t index) {
    return syscall68k(sd, flags, SYSCALL_68K_DM_GET_DATABASE, false, [=]() {
        pacePush16(index);
        pacePush16(cardNo);
    });
}

uint16_t syscall68k_DmDatabaseInfo(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                   uint32_t dbId, uint32_t nameP, uint32_t attributesP,
                                   uint32_t versionP, uint32_t crDateP, uint32_t modDateP,
                                   uint32_t bcpUpDateP, uint32_t modNumP, uint32_t appInfoIDP,
                                   uint32_t sortInfoIDP, uint32_t typeP, uint32_t creatorP) {
    return syscall68k(sd, flags, SYSCALL_68K_DM_DATABASE_INFO, false, [=]() {
        pacePush32(creatorP);
        pacePush32(typeP);
        pacePush32(sortInfoIDP);
        pacePush32(appInfoIDP);
        pacePush32(modNumP);
        pacePush32(bcpUpDateP);
        pacePush32(modDateP);
        pacePush32(crDateP);
        pacePush32(versionP);
        pacePush32(attributesP);
        pacePush32(nameP);
        pacePush32(dbId);
        pacePush16(cardNo);
    });
}

uint16_t syscall68k_DmDatabaseProtect(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                      uint32_t dbID, uint8_t protect) {
    return syscall68k(sd, flags, SYSCALL_68K_DM_DATABASE_PROTECT, false, [=]() {
        pacePush8(protect);
        pacePush32(dbID);
        pacePush16(cardNo);
    });
}

uint16_t syscall68k_DmDeleteDatabase(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                     uint32_t dbID) {
    return syscall68k(sd, flags, SYSCALL_68K_DM_DELETE_DATABASE, false, [=]() {
        pacePush32(dbID);
        pacePush16(cardNo);
    });
}

uint32_t syscall68k_DmFindDatabase(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                   uint32_t nameP) {
    return syscall68k(sd, flags, SYSCALL_68K_DM_FIND_DATABASE, false, [=]() {
        pacePush32(nameP);
        pacePush16(cardNo);
    });
}

uint16_t syscall68k_SysUIAppSwitch(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                   uint32_t dbID, uint16_t cmd, uint32_t cmdPBP) {
    return syscall68k(sd, flags, SYSCALL_68K_SYS_UI_APP_SWITCH, false, [=]() {
        pacePush32(cmdPBP);
        pacePush16(cmd);
        pacePush32(dbID);
        pacePush16(cardNo);
    });
}

uint16_t syscall68k_SysCurAppDatabase(struct SyscallDispatch* sd, uint32_t flags, uint32_t cardNoP,
                                      uint32_t dbIDP) {
    return syscall68k(sd, flags, SYSCALL_68K_SYS_CUR_APP_DATABASE, false, [=]() {
        pacePush32(dbIDP);
        pacePush32(cardNoP);
    });
}

uint16_t syscall68k_SysAppLaunch(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo, uint32_t dbID,
                                 uint16_t launchFlags, uint16_t cmd, uint32_t cmdPBP, uint32_t resultP) {
    return syscall68k(sd, flags, SYSCALL_68K_SYS_APP_LAUNCH, false, [=]() {
        pacePush32(resultP);
        pacePush32(cmdPBP);
        pacePush16(cmd);
        pacePush16(launchFlags);
        pacePush32(dbID);
        pacePush16(cardNo);
    });
}

uint16_t syscall68k_DmGetNextDatabaseByTypeCreator(struct SyscallDispatch* sd, uint32_t flags, bool newSearch,
                                                   uint32_t stateP, uint32_t type, uint32_t creator,
                                                   bool onlyLatestVersion, uint32_t cardNoP, uint32_t dbIDP) {
    return syscall68k(sd, flags, SYSCALL_68K_DM_GET_NEXT_DATABASE_BY_TYPE_CREATOR, false, [=]() {
        pacePush32(dbIDP);
        pacePush32(cardNoP);
        pacePush8(onlyLatestVersion);
        pacePush32(creator);
        pacePush32(type);
        pacePush32(stateP);
        pacePush8(newSearch);
    });
}

uint16_t syscall68k_SysNotifyBroadcast(struct SyscallDispatch* sd, uint32_t flags, uint32_t notifyP) {
    return syscall68k(sd, flags, SYSCALL_68K_SYS_NOTIFY_BROADCAST, false, [=]() { pacePush32(notifyP); });
}

bool syscall68k_EvtEventAvail(struct SyscallDispatch* sd, uint32_t flags) {
    return syscall68k(sd, flags, SYSCALL_68K_EVT_EVENT_AVAIL, false, []() {}) & 0xff;
}

bool syscall68k_EvtSysEventAvail(struct SyscallDispatch* sd, uint32_t flags, bool ignorePenUps) {
    return syscall68k(sd, flags, SYSCALL_68K_EVT_SYS_EVENT_AVAIL, false, [=]() { pacePush8(ignorePenUps); }) & 0xff;
}

// Palm SDK 5r4 VFSMgr.h defines selector values; PalmTypes.h places the selector in D2.
static uint16_t syscall68k_vfs(SyscallDispatch* sd, uint32_t flags, uint16_t selector,
                              std::function<void()> arguments) {
    if (!(flags & SC_EXECUTE_FULL)) throw std::runtime_error("Filesystem calls need full guest execution");
    flags |= SC_EXECUTE_TIMERS;
    const bool trace = std::getenv("FROND_VFS_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "VFS selector %u begin\n", unsigned(selector));
    uint16_t result;
    try { result = syscall68k(sd, flags, 0xa348, false, [=]() {
        paceSetDreg(2, selector);
        arguments();
        if (trace) {
            uint8_t bytes[16]{};
            const bool readable = cpuInspectRead(sd->soc->GetCpu(), paceGetAreg(7), bytes, sizeof(bytes));
            std::fprintf(stderr, "VFS raw D2=%08x SP=%08x readable=%u bytes=", paceGetDreg(2), paceGetAreg(7), unsigned(readable));
            for (uint8_t byte : bytes) std::fprintf(stderr, "%02x", unsigned(byte));
            std::fprintf(stderr, "\n");
        }
    }); } catch (...) {
        if (trace) {
            const uint32_t* r = cpuGetRegisters(sd->soc->GetCpu());
            std::fprintf(stderr, "VFS selector %u interrupted pc=%08x lr=%08x r0=%08x r1=%08x r2=%08x sp=%08x\n", unsigned(selector), r[15], r[14], r[0], r[1], r[2], r[13]);
            std::fprintf(stderr, "VFS interrupted r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x cpsr=%08x\n", r[3], r[4], r[5], r[6], r[7], cpuGetRegExternal(sd->soc->GetCpu(), ARM_REG_NUM_CPSR));
            uint32_t global = 0;
            if (cpuInspectRead(sd->soc->GetCpu(), r[4] + 0x14, &global, sizeof(global))) {
                uint32_t words[5]{};
                const bool readable = cpuInspectRead(sd->soc->GetCpu(), global + 0x21c0, words, sizeof(words));
                std::fprintf(stderr, "VFS global=%08x readyReadable=%u readyHead=%08x readyTail=%08x words=%08x,%08x,%08x\n", global, unsigned(readable), words[0], words[1], words[2], words[3], words[4]);
            }
        }
        throw;
    }
    if (trace) std::fprintf(stderr, "VFS selector %u result %04x\n", unsigned(selector), unsigned(result));
    return result;
}
uint16_t syscall68k_VFSVolumeEnumerate(SyscallDispatch* sd, uint32_t flags, uint32_t volumeP, uint32_t iteratorP) {
    return syscall68k_vfs(sd, flags, 27, [=]() { pacePush32(iteratorP); pacePush32(volumeP); });
}
uint16_t syscall68k_VFSVolumeInfo(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t infoP) {
    return syscall68k_vfs(sd, flags, 28, [=]() { pacePush32(infoP); pacePush16(volume); });
}
uint16_t syscall68k_VFSVolumeGetLabel(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t labelP, uint16_t length) {
    return syscall68k_vfs(sd, flags, 29, [=]() { pacePush16(length); pacePush32(labelP); pacePush16(volume); });
}
uint16_t syscall68k_VFSVolumeSize(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t usedP, uint32_t totalP) {
    return syscall68k_vfs(sd, flags, 31, [=]() { pacePush32(totalP); pacePush32(usedP); pacePush16(volume); });
}
uint16_t syscall68k_VFSVolumeFormat(SyscallDispatch* sd, uint32_t flags, uint8_t formatFlags, uint16_t fsLibrary, uint32_t mountParametersP) {
    return syscall68k_vfs(sd, flags, 24, [=]() { pacePush32(mountParametersP); pacePush16(fsLibrary); pacePush8(formatFlags); });
}
uint16_t syscall68k_VFSFileCreate(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t pathP) {
    return syscall68k_vfs(sd, flags, 2, [=]() { pacePush32(pathP); pacePush16(volume); });
}
uint16_t syscall68k_VFSFileOpen(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t pathP, uint16_t mode, uint32_t fileP) {
    return syscall68k_vfs(sd, flags, 3, [=]() { pacePush32(fileP); pacePush16(mode); pacePush32(pathP); pacePush16(volume); });
}
uint16_t syscall68k_VFSFileClose(SyscallDispatch* sd, uint32_t flags, uint32_t file) {
    return syscall68k_vfs(sd, flags, 4, [=]() { pacePush32(file); });
}
uint16_t syscall68k_VFSFileRead(SyscallDispatch* sd, uint32_t flags, uint32_t file, uint32_t length, uint32_t bytesP, uint32_t countP) {
    return syscall68k_vfs(sd, flags, 6, [=]() { pacePush32(countP); pacePush32(bytesP); pacePush32(length); pacePush32(file); });
}
uint16_t syscall68k_VFSFileWrite(SyscallDispatch* sd, uint32_t flags, uint32_t file, uint32_t length, uint32_t bytesP, uint32_t countP) {
    return syscall68k_vfs(sd, flags, 7, [=]() { pacePush32(countP); pacePush32(bytesP); pacePush32(length); pacePush32(file); });
}
uint16_t syscall68k_VFSFileDelete(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t pathP) {
    return syscall68k_vfs(sd, flags, 8, [=]() { pacePush32(pathP); pacePush16(volume); });
}
uint16_t syscall68k_VFSFileRename(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t pathP, uint32_t nameP) {
    return syscall68k_vfs(sd, flags, 9, [=]() { pacePush32(nameP); pacePush32(pathP); pacePush16(volume); });
}
uint16_t syscall68k_VFSFileSize(SyscallDispatch* sd, uint32_t flags, uint32_t file, uint32_t sizeP) {
    return syscall68k_vfs(sd, flags, 18, [=]() { pacePush32(sizeP); pacePush32(file); });
}
uint16_t syscall68k_VFSDirCreate(SyscallDispatch* sd, uint32_t flags, uint16_t volume, uint32_t pathP) {
    return syscall68k_vfs(sd, flags, 19, [=]() { pacePush32(pathP); pacePush16(volume); });
}
uint16_t syscall68k_VFSDirEntryEnumerate(SyscallDispatch* sd, uint32_t flags, uint32_t directory, uint32_t iteratorP, uint32_t infoP) {
    return syscall68k_vfs(sd, flags, 20, [=]() { pacePush32(infoP); pacePush32(iteratorP); pacePush32(directory); });
}

uint16_t syscall68k_VFSFileGetAttributes(SyscallDispatch* sd, uint32_t flags, uint32_t file, uint32_t attributesP) {
    return syscall68k_vfs(sd, flags, 14, [=]() { pacePush32(attributesP); pacePush32(file); });
}
