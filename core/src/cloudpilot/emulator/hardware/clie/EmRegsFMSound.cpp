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
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"

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

EmRegsFMSound::EmRegsFMSound(emuptr baseAddr, bool enabled) : enabled(enabled), fBaseAddr(baseAddr) {
    sound.onInterrupt = [this](bool asserted, bool restoring) {
        if (onInterrupt) onInterrupt(asserted, restoring);
    };
    sound.onSample = [this](uint64 cycle, int16 sample, bool fm) {
        if (onSample) onSample(cycle, sample, fm);
    };
}

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
    if (enabled) EmHAL::AddCycleConsumer(Cycle, this);
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
    if (enabled) sound.Reset(gCPU68K->CycleNow());
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
    if (enabled) EmHAL::RemoveCycleConsumer(Cycle, this);
    sound.SetTrace(nullptr, 0);
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
        sound.SelectRegister(uint8(value));
    } else if ((offset == 2 && size == 2) || (offset == 3 && size == 1)) {
        sound.Write(gCPU68K->CycleNow(), EmHAL::GetSystemClockFrequency(), uint8(value));
    }
}

uint32 EmRegsFMSound::ReadFromDummy(emuptr address, int size) {
    const auto offset = address - fBaseAddr;
    if (!enabled || !((offset == 2 && size == 2) || (offset == 3 && size == 1))) return 0;
    return sound.Read(gCPU68K->CycleNow(), EmHAL::GetSystemClockFrequency());
}

void EmRegsFMSound::Save(Savestate<ChunkType>& state) { SaveState(state); }
void EmRegsFMSound::Save(SavestateProbe<ChunkType>& state) { SaveState(state); }

template <typename T>
void EmRegsFMSound::SaveState(T& state) {
    if (!enabled) return;
    if (auto* chunk = state.GetChunk(ChunkType::regsPa1Sound)) sound.Save(*chunk);
}

void EmRegsFMSound::Load(SavestateLoader<ChunkType>& loader) {
    // Earlier T415 sessions start with a reset sound chip.
    if (!enabled || !loader.HasChunk(ChunkType::regsPa1Sound)) return;
    auto* chunk = loader.GetChunk(ChunkType::regsPa1Sound);
    const auto version = chunk->Get32();
    chunk->Reset();
    if (version > 1) EmHAL::loadedNewerState = true;
    if (!sound.Load(*chunk, gSession->GetSystemCycles())) loader.NotifyError();
}

bool EmRegsFMSound::Enabled() {
    const auto* sound = dynamic_cast<EmRegsFMSound*>(EmBankRegs::GetSubBank(FMSound_BaseAddress, 2));
    return sound && sound->enabled;
}

void EmRegsFMSound::SetTrace(FILE* output) { sound.SetTrace(output, gCPU68K->CycleNow()); }

void EmRegsFMSound::RestoreOutput(uint64 cycles) { sound.RestoreOutput(cycles); }

void EmRegsFMSound::Cycle(void* context, uint64 cycles, Bool) {
    static_cast<EmRegsFMSound*>(context)->sound.Advance(cycles, EmHAL::GetSystemClockFrequency());
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
