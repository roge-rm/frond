/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
        Copyright (c) 2000-2001 Palm, Inc. or its subsidiaries.
        All rights reserved.

        This file is part of the Palm OS Emulator.

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.
\* ===================================================================== */

#ifndef EmSPISlaveADS784x_h
#define EmSPISlaveADS784x_h

#include "EmSPISlave.h"  // EmSPISlave

enum EmADSChannelType {
    kChannelUnused,
    kChannelPenX,
    kChannelPenY,
    kChannelBattery7843,
    kChannelBattery7846,
    kChannelDockSerial,
    kChannelDockTwister,
    kChannelTemp0,
    kChannelTemp1,
    kChannelVirtualInput
};

#define kChannelSet1                                                                  \
    kChannelTemp0, kChannelPenY, kChannelBattery7846, kChannelUnused, kChannelUnused, \
        kChannelPenX, kChannelDockTwister, kChannelTemp1

#define kChannelSet2                                                                   \
    kChannelUnused, kChannelPenY, kChannelBattery7843, kChannelUnused, kChannelUnused, \
        kChannelPenX, kChannelDockSerial, kChannelUnused

#define kChannelSet3                                                                   \
    kChannelUnused, kChannelPenY, kChannelBattery7843, kChannelUnused, kChannelUnused, \
        kChannelPenX, kChannelDockTwister, kChannelUnused

class EmSPISlaveADS784x : public EmSPISlave {
   public:
    EmSPISlaveADS784x(EmADSChannelType ch0, EmADSChannelType ch1, EmADSChannelType ch2,
                      EmADSChannelType ch3, EmADSChannelType ch4, EmADSChannelType ch5,
                      EmADSChannelType ch6, EmADSChannelType ch7);
    virtual ~EmSPISlaveADS784x(void);

    virtual void Save(Savestate<ChunkType>&);
    virtual void Save(SavestateProbe<ChunkType>&);
    virtual void Load(SavestateLoader<ChunkType>&);

    virtual uint16 DoExchange(uint16 control, uint16 data);
    virtual void Reset(void);

    // 12-bit reading of the kChannelDockSerial ID input; 0 (undocked) unless the device sets it.
    // Not saved: whoever owns the cradle sets it again after a load.
    void SetDockLevel(uint16 level) { fDockLevel = level; }

   protected:
    virtual uint16 ReadVirtualInput(unsigned channel) const { return 0; }
    void ProcessCommand(uint8);
    void LoadPendingConversion(void);

    template <typename T>
    void DoSave(T& savestate);

    template <typename T>
    void DoSaveLoad(T& helper);

   private:
    EmADSChannelType fChannelUse[8];
    uint16 fDockLevel{0};

    uint32 fBitBufferIn;
    uint16 fBitBufferOut;
    int fNumBitsIn;
    uint16 fPendingResult;
    Bool fHavePending;

    int fCommandBitsSeen;
};

#endif  // EmSPISlaveADS784x_h
