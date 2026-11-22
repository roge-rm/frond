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

#ifndef EmRegsFMSound_h
#define EmRegsFMSound_h

#include <cstdio>
#include <functional>

#include "EmPalmStructs.h"
#include "EmRegs.h"
#include "audio/SonyMa2Sound.h"

#define FMSound_BaseAddress 0x10980000
#define FMSound_Range 0x0000FFFF

typedef struct tagHwrFMSound {
    UInt8 _filler01[FMSound_Range];  // 0x000-FFFF
} HwrFMSoundType;

class EmRegsFMSound : public EmRegs {
   public:
    explicit EmRegsFMSound(emuptr baseAddr, bool enabled = false);
    virtual ~EmRegsFMSound(void);

    virtual void Initialize(void);
    virtual void Reset(Bool hardwareReset);
    virtual void Dispose(void);

    virtual void SetSubBankHandlers(void);
    virtual uint8* GetRealAddress(emuptr address);
    virtual emuptr GetAddressStart(void);
    virtual uint32 GetAddressRange(void);

    void SetTrace(FILE* output);
    static bool Enabled();
    void Save(Savestate<ChunkType>& state) override;
    void Save(SavestateProbe<ChunkType>& state) override;
    void Load(SavestateLoader<ChunkType>& loader) override;
    void RestoreOutput(uint64 cycles);
    std::function<void(bool asserted, bool restoring)> onInterrupt;
    std::function<void(uint64, int16, bool)> onSample;

    void UnsupportedWrite(emuptr address, int size, uint32 value);

    virtual bool AllowUnalignedAccess(emuptr address, int size);

   private:
    uint32 ReadFromDummy(emuptr address, int size);
    void WriteToDummy(emuptr address, int size, uint32 value);

    int ValidAddress(emuptr iAddress, uint32 iSize);

    template <typename T> void SaveState(T& state);
    static void Cycle(void* context, uint64 cycles, Bool sleeping);

    const bool enabled;
    SonyMa2Sound sound;
    HwrFMSoundType fRegs;
    emuptr fBaseAddr;
};

#endif /* EmRegsFMSound_h */
