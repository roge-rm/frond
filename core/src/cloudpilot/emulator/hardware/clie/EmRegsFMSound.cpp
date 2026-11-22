/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
        Copyright (c) 1998-2000 Palm, Inc. or its subsidiaries.
        All rights reserved.

        This file is part of the Palm OS Emulator.

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.
\* ===================================================================== */

#include "EmRegsFMSound.h"

#include "Byteswapping.h"  // ByteswapWords
#include "EmBankROM.h"     // ROMBank::IsPCInRAM
#include "EmBankRegs.h"    // RegsBank::GetROMSize
#include "EmCommon.h"
#include "EmCPU68K.h"
#include "EmHAL.h"
#include "EmMemory.h"  // gMemoryAccess
#include "EmSession.h"
#include "SonyMa2Fm.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"

#include <algorithm>
#include <array>
#include <limits>

#define WAITSTATES_USB 0

typedef uint32 (*ReadFunction)(emuptr address, int size);
typedef void (*WriteFunction)(emuptr address, int size, uint32 value);

//*******************************************************************
// EmRegsFMSound Class
//*******************************************************************

// Macro to return the Dragonball address of the specified register

#define addressof(x) (this->GetAddressStart() + offsetof(HwrFMSoundType, x))

#define INSTALL_HANDLER(read, write, reg)                                                      \
    this->SetHandler((ReadFunction)&EmRegsFMSound::read, (WriteFunction)&EmRegsFMSound::write, \
                     addressof(reg), sizeof(fRegs.reg))
#pragma mark -

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::EmRegsFMSound
// ---------------------------------------------------------------------------

EmRegsFMSound::EmRegsFMSound(emuptr baseAddr, bool enabled) : enabled(enabled), fBaseAddr(baseAddr) {}

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::~EmRegsFMSound
// ---------------------------------------------------------------------------

EmRegsFMSound::~EmRegsFMSound(void) {}

/***********************************************************************
 *
 * FUNCTION:	EmRegsFMSound::Initialize
 *
 * DESCRIPTION: Standard initialization function.  Responsible for
 *				initializing this sub-system when a new session is
 *				created.  May also be called from the Load function
 *				to share common functionality.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmRegsFMSound::Initialize(void) {
    EmRegs::Initialize();
    if (enabled) EmHAL::AddCycleConsumer(TraceCycle, this);
}

/***********************************************************************
 *
 * FUNCTION:	EmRegsFMSound::Reset
 *
 * DESCRIPTION: Standard reset function.  Sets the sub-system to a
 *				default state.	This occurs not only on a Reset (as
 *				from the menu item), but also when the sub-system
 *				is first initialized (Reset is called after Initialize)
 *				as well as when the system is re-loaded from an
 *				insufficient session file.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmRegsFMSound::Reset(Bool hardwareReset) {
    EmRegs::Reset(hardwareReset);
    if (enabled) ResetTrace(gCPU68K->CycleNow());
    if (hardwareReset) {
        memset(&fRegs, 0, sizeof(fRegs));
    }
}

/***********************************************************************
 *
 * FUNCTION:	EmRegsFMSound::Dispose
 *
 * DESCRIPTION: Standard dispose function.	Completely release any
 *				resources acquired or allocated in Initialize and/or
 *				Load.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmRegsFMSound::Dispose(void) {
    if (enabled) EmHAL::RemoveCycleConsumer(TraceCycle, this);
    trace = nullptr;
}

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::SetSubBankHandlers
// ---------------------------------------------------------------------------
void EmRegsFMSound::SetSubBankHandlers(void) {
    // Install base handlers.

    EmRegs::SetSubBankHandlers();

    // Now add standard/specialized handers for the defined registers.

    INSTALL_HANDLER(ReadFromDummy, WriteToDummy, _filler01);
}

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::GetRealAddress
// ---------------------------------------------------------------------------

uint8* EmRegsFMSound::GetRealAddress(emuptr address) {
    uint8* loc = ((uint8*)&fRegs) + (address - this->GetAddressStart());
    return loc;
}

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::GetAddressStart
// ---------------------------------------------------------------------------

emuptr EmRegsFMSound::GetAddressStart(void) { return fBaseAddr; }

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::GetAddressRange
// ---------------------------------------------------------------------------

uint32 EmRegsFMSound::GetAddressRange(void) { return sizeof(fRegs); }

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::WrireToDummy
// ---------------------------------------------------------------------------
void EmRegsFMSound::WriteToDummy(emuptr address, int size, uint32 value) {
    if (!enabled) return;
    const auto offset = address - fBaseAddr;
    if ((offset == 0 && size == 2) || (offset == 1 && size == 1)) {
        index = uint8(value);
        return;
    }
    if (!((offset == 2 && size == 2) || (offset == 3 && size == 1))) return;
    const auto now = gCPU68K->CycleNow();
    const auto data = uint8(value);
    AdvanceAdpcm(now);
    AdvanceFm(now);
    LogTrace(now, 'w', page, index, data);
    if (index == 0x0f) {
        page = data & 1;
        return;
    }

    const auto previous = registers[page][index];
    registers[page][index] = data;
    WriteFm(now, data);
    if (page == 0 && index == 0x00 && sequenceBytes < 96) ++sequenceBytes;
    if (page == 0 && index == 0x05 && adpcmFifo.size() < 384) adpcmFifo.push_back(data);
    if (page == 1 && index == 0x01 && ((previous ^ data) & 4)) {
        adpcmDeadline = (data & 4) ? now : 0;
        if (!(data & 4)) {
            adpcmSampleHeld = false;
            OutputSample(now, 0, false);
        }
    }
    if (page == 0 && index == 0x06) {
        if ((data & 0x11) != 0x11) {
            sequenceDeadline = 0;
            sequencePending = false;
        } else if ((previous & 0x11) != 0x11) {
            // Pa1Lib advances SMF time by 20 ms for each 01 FF 00 timer packet.
            sequencePeriod = std::max(1, EmHAL::GetSystemClockFrequency() / 50);
            sequenceDeadline = now + sequencePeriod;
        }
    }
    if (page == 1 && index == 0x04 && (data & 0x80)) ResetAdpcm(now);
    if (page == 1 && index == 0x04 && (data & 0x60)) {
        sequenceBytes = 0;
        sequencePending = false;
    }
    UpdateTraceIrq(now);
}

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::ReadFromDummy
// ---------------------------------------------------------------------------
uint32 EmRegsFMSound::ReadFromDummy(emuptr address, int size) {
    if (!enabled) return 0;
    const auto offset = address - fBaseAddr;
    if (!((offset == 2 && size == 2) || (offset == 3 && size == 1))) return 0;
    const auto now = gCPU68K->CycleNow();
    AdvanceAdpcm(now);
    auto value = registers[page][index];
    if (page == 1 && index == 0x0e) value = 3;
    if (page == 0 && index == 0x08) value = sequencePending ? 1 : 0;
    if (page == 0 && index == 0x0a) value = AdpcmStatus();
    LogTrace(now, 'r', page, index, value);
    if (page == 0 && index == 0x08) {
        sequencePending = false;
        UpdateTraceIrq(now);
    }
    return value;
}

void EmRegsFMSound::Save(Savestate<ChunkType>& state) { SaveState(state); }
void EmRegsFMSound::Save(SavestateProbe<ChunkType>& state) { SaveState(state); }

template <typename T>
void EmRegsFMSound::SaveState(T& state) {
    if (!enabled) return;
    auto* chunk = state.GetChunk(ChunkType::regsPa1Sound);
    if (!chunk) return;
    chunk->Put32(1);
    chunk->PutBuffer(registers, sizeof(registers));
    chunk->Put32(page);
    chunk->Put32(index);
    chunk->Put32(sequenceBytes);
    chunk->Put64(sequenceDeadline);
    chunk->Put64(sequencePeriod);
    chunk->PutBool(sequencePending);

    std::array<uint8, 384> fifo{};
    std::copy(adpcmFifo.begin(), adpcmFifo.end(), fifo.begin());
    chunk->Put32(adpcmFifo.size());
    chunk->PutBuffer(fifo.data(), fifo.size());
    chunk->Put64(adpcmDeadline);
    chunk->Put32(uint32(adpcmLevel));
    chunk->Put32(adpcmStep);
    chunk->Put8(adpcmByte);
    chunk->PutBool(adpcmSecondNibble);
    chunk->PutBool(adpcmSampleHeld);
    chunk->Put16(uint16(adpcmSample));

    chunk->Put64(fmDeadline);
    chunk->Put64(fmCycle);
    chunk->Put16(uint16(fmSample));
    auto engine = fm->Save();
    chunk->PutBuffer(engine.data(), engine.size());
}

void EmRegsFMSound::Load(SavestateLoader<ChunkType>& loader) {
    // Earlier T415 sessions start with a reset sound chip.
    if (!enabled || !loader.HasChunk(ChunkType::regsPa1Sound)) return;
    auto* chunk = loader.GetChunk(ChunkType::regsPa1Sound);
    const auto version = chunk->Get32();
    if (version != 1) {
        if (version > 1) EmHAL::loadedNewerState = true;
        loader.NotifyError();
        return;
    }
    chunk->GetBuffer(registers, sizeof(registers));
    page = chunk->Get32();
    index = chunk->Get32();
    sequenceBytes = chunk->Get32();
    sequenceDeadline = chunk->Get64();
    sequencePeriod = chunk->Get64();
    sequencePending = chunk->GetBool();

    std::array<uint8, 384> fifo;
    const auto count = chunk->Get32();
    chunk->GetBuffer(fifo.data(), fifo.size());
    adpcmDeadline = chunk->Get64();
    adpcmLevel = int32(chunk->Get32());
    adpcmStep = chunk->Get32();
    adpcmByte = chunk->Get8();
    adpcmSecondNibble = chunk->GetBool();
    adpcmSampleHeld = chunk->GetBool();
    adpcmSample = int16(chunk->Get16());

    fmDeadline = chunk->Get64();
    fmCycle = chunk->Get64();
    fmSample = int16(chunk->Get16());
    std::vector<uint8> engine(SonyMa2Fm::StateSize());
    chunk->GetBuffer(engine.data(), engine.size());

    const auto now = gSession->GetSystemCycles();
    const auto near = [now](uint64 cycle) {
        constexpr uint64 maxDistance = 1ull << 27;
        return !cycle || (now < std::numeric_limits<uint64>::max() - 2 * maxDistance &&
                          (cycle > now ? cycle - now : now - cycle) <= maxDistance);
    };
    if (!chunk->IsAtEnd() || page > 1 || index > 255 || sequenceBytes > 96 ||
        count > fifo.size() || adpcmLevel < -32768 || adpcmLevel > 32767 || adpcmStep < 127 ||
        adpcmStep > 24576 || (sequenceDeadline && !sequencePeriod) || sequencePeriod > (1ull << 27) ||
        !near(sequenceDeadline) || !near(adpcmDeadline) || !near(fmDeadline) ||
        (fmDeadline && (fmCycle > fmDeadline || !near(fmCycle) || fmDeadline - fmCycle > 65536)) ||
        !fm->Load(engine)) {
        loader.NotifyError();
        return;
    }
    adpcmFifo.assign(fifo.begin(), fifo.begin() + count);
    // VZ's interrupt latch was already restored with the rest of its registers.
    irqAsserted = sequencePending && (registers[1][0x01] & 1);
    if (onInterrupt) onInterrupt(irqAsserted, true);
}

bool EmRegsFMSound::Enabled() {
    const auto* sound = dynamic_cast<EmRegsFMSound*>(EmBankRegs::GetSubBank(FMSound_BaseAddress, 2));
    return sound && sound->enabled;
}

void EmRegsFMSound::SetTrace(FILE* output) {
    trace = output;
    if (trace) LogTrace(gCPU68K->CycleNow(), 's', page, index, 0);
}

void EmRegsFMSound::RestoreOutput(uint64 cycles) {
    if (!onSample) return;
    onSample(cycles, adpcmSample, false);
    onSample(cycles, fmSample, true);
}

void EmRegsFMSound::OutputSample(uint64 cycles, int sample, bool isFm) {
    const int speaker = registers[1][0x25];
    auto& current = isFm ? fmSample : adpcmSample;
    // Yamaha's 5-bit analogue gain codes give mute, then -30 to 0 dB in 1 dB steps.
    constexpr unsigned gain[] = {
        0, 1036, 1163, 1305, 1464, 1642, 1843, 2068,
        2320, 2603, 2920, 3277, 3677, 4125, 4629, 5193,
        5827, 6538, 7336, 8231, 9235, 10362, 11627, 13045,
        14637, 16423, 18427, 20675, 23198, 26029, 29205, 32768,
    };
    const int level = (speaker & 0x80) ? speaker & 31 : 0;
    const int eq = registers[1][0x22] & 31;
    const auto scaled = int64_t(sample) * gain[level] * gain[eq] / (1ll << 30);
    current = int16(std::clamp<int64_t>(scaled, -32768, 32767));
    if (onSample) onSample(cycles, current, isFm);
}

void EmRegsFMSound::TraceCycle(void* context, uint64 cycles, Bool) {
    static_cast<EmRegsFMSound*>(context)->AdvanceTrace(cycles);
}

void EmRegsFMSound::AdvanceTrace(uint64 cycles) {
    AdvanceAdpcm(cycles);
    AdvanceFm(cycles);
    if (!sequenceDeadline || cycles < sequenceDeadline) return;
    const auto elapsed = 1 + (cycles - sequenceDeadline) / sequencePeriod;
    const auto retired = std::min<uint64>(elapsed, sequenceBytes / 3);
    sequenceBytes -= unsigned(retired) * 3;
    sequenceDeadline += elapsed * sequencePeriod;
    if (retired && sequenceBytes < 18) sequencePending = true;
    UpdateTraceIrq(cycles);
}

uint8 EmRegsFMSound::AdpcmStatus() const {
    const bool room = adpcmFifo.size() <= 384 - 96;
    const bool finished = adpcmFifo.empty() && !adpcmSecondNibble && !adpcmSampleHeld;
    return (room ? 1 : 0) | (finished ? 4 : 0);
}

void EmRegsFMSound::ResetAdpcm(uint64 cycles) {
    adpcmFifo.clear();
    adpcmDeadline = 0;
    adpcmSecondNibble = adpcmSampleHeld = false;
    adpcmLevel = 0;
    adpcmStep = 127;
    OutputSample(cycles, 0, false);
}

void EmRegsFMSound::AdvanceAdpcm(uint64 cycles) {
    if (!adpcmDeadline || cycles < adpcmDeadline) return;
    const int rate = (registers[1][0x01] & 8) ? 8000 : 4000;
    const auto period = std::max(1, EmHAL::GetSystemClockFrequency() / rate);
    while (adpcmDeadline <= cycles) {
        if (!adpcmSecondNibble && adpcmFifo.empty()) {
            adpcmSampleHeld = false;
            OutputSample(adpcmDeadline, 0, false);
            adpcmDeadline = cycles + period;
            break;
        }
        if (!adpcmSecondNibble) {
            adpcmByte = adpcmFifo.front();
            adpcmFifo.pop_front();
        }
        const unsigned nibble = adpcmSecondNibble ? adpcmByte >> 4 : adpcmByte & 15;
        adpcmSecondNibble = !adpcmSecondNibble;
        adpcmSampleHeld = true;

        // Yamaha 4-bit ADPCM, as in ymfm's ADPCM-B decoder.
        constexpr int stepScale[] = {57, 57, 57, 57, 77, 102, 128, 153};
        const int delta = ((2 * int(nibble & 7) + 1) * adpcmStep) / 8;
        adpcmLevel = std::clamp(adpcmLevel + ((nibble & 8) ? -delta : delta), -32768, 32767);
        adpcmStep = std::clamp(adpcmStep * stepScale[nibble & 7] / 64, 127, 24576);
        const int sample = adpcmLevel * (registers[1][0x21] & 31) * (registers[1][0x10] & 31) / (31 * 31);
        OutputSample(adpcmDeadline, sample, false);
        adpcmDeadline += period;
    }
}

// The Yamaha voice and channel fields match the sampler already used by the Zodiac.
void EmRegsFMSound::WriteFm(uint64 cycles, uint8 data) {
    if (page == 0 && index == 0x06) {
        if ((data & 0x11) == 0x11) {
            if (!fmDeadline) fmCycle = fmDeadline = cycles;
        } else {
            fmDeadline = 0;
            OutputSample(cycles, 0, true);
        }
        return;
    }
    if (index < 0x10) return;
    if (page == 1 && (index == 0x30 || index == 0x31 || index == 0x38)) return;

    fm->Write(page, index, data);
}

void EmRegsFMSound::AdvanceFm(uint64 cycles) {
    if (!fmDeadline || cycles < fmDeadline) return;
    const uint64 clock = EmHAL::GetSystemClockFrequency();
    const uint64 period = std::max<uint64>(1, clock / 44100);
    while (fmDeadline <= cycles) {
        int32_t left = 0, right = 0;
        fm->Advance(fmDeadline - fmCycle, clock);
        fm->Output(left, right);
        const int sample = ((left + right) / 2) * (registers[1][0x20] & 31) / 31;
        OutputSample(fmDeadline, sample, true);
        fmCycle = fmDeadline;
        fmDeadline += period;
    }
}

void EmRegsFMSound::UpdateTraceIrq(uint64 cycles) {
    const bool asserted = sequencePending && (registers[1][0x01] & 1);
    if (irqAsserted == asserted) return;
    irqAsserted = asserted;
    LogTrace(cycles, 'i', 0, 0x08, asserted);
    if (onInterrupt) onInterrupt(asserted, false);
}

void EmRegsFMSound::ResetTrace(uint64 cycles) {
    memset(registers, 0, sizeof(registers));
    page = index = sequenceBytes = 0;
    ResetAdpcm(cycles);
    fm = std::make_unique<SonyMa2Fm>();
    fmDeadline = fmCycle = 0;
    OutputSample(cycles, 0, true);
    sequenceDeadline = sequencePeriod = 0;
    sequencePending = false;
    UpdateTraceIrq(cycles);
    LogTrace(cycles, 's', 0, 0, 0);
}

void EmRegsFMSound::LogTrace(uint64 cycles, char operation, unsigned bank, unsigned reg, unsigned value) {
    if (!trace) return;
    fprintf(trace, "%llu,%c,%u,%02x,%02x\n", static_cast<unsigned long long>(cycles), operation, bank, reg,
            value);
}

// ---------------------------------------------------------------------------
//		� EmRegsFMSound::ValidAddress
// ---------------------------------------------------------------------------

int EmRegsFMSound::ValidAddress(emuptr address, uint32 iSize) {
    UNUSED_PARAM(iSize)
    return 1;
}

// ---------------------------------------------------------------------------
//		� USBSonyReg::UnsupportedWrite
// ---------------------------------------------------------------------------

void EmRegsFMSound::UnsupportedWrite(emuptr address, int size, uint32 value) {
    UNUSED_PARAM(value)

    return;
}

bool EmRegsFMSound::AllowUnalignedAccess(emuptr address, int size) {
    UNUSED_PARAM(address);
    UNUSED_PARAM(size);

    return true;
}

#pragma mark -
