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

#ifndef EmRegs328PalmPilot_h
#define EmRegs328PalmPilot_h

#include <memory>

#include "EmRegs328.h"  // EmRegs328

class EmSPISlaveADS784x;

class EmRegs328PalmPilot : public EmRegs328 {
   public:
    EmRegs328PalmPilot();
    virtual ~EmRegs328PalmPilot();

    virtual void Reset(Bool hardwareReset);
    virtual void Save(Savestate<ChunkType>&);
    virtual void Save(SavestateProbe<ChunkType>&);
    virtual void Load(SavestateLoader<ChunkType>&);

    virtual Bool GetLCDScreenOn(void);
    virtual Bool GetLCDBacklightOn(void);
    virtual Bool GetLineDriverState(EmUARTDeviceType);

    virtual uint8 GetPortInputValue(int);

   protected:
    virtual EmSPISlave* GetSPISlave(void);

   private:
    bool RunsIdDetect();

    std::unique_ptr<EmSPISlaveADS784x> fSPISlaveADC;

    // -1 until there is a session to ask.
    int fIdDetect{-1};
};

#endif /* EmRegs328PalmPilot_h */
