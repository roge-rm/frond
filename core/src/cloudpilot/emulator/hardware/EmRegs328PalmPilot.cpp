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

#include "EmRegs328PalmPilot.h"

#include "EmCommon.h"
#include "EmRegs328Prv.h"
#include "EmSession.h"  // GetDevice
#include "EmBattery.h"  // gBattery
#include "EmSPISlaveADS784x.h"
#include "PalmPack.h"
#define NON_PORTABLE
#include "328Jerry/IncsPrv/HardwareTD1.h"  // hwrTD1PortFLCDEnableOn, etc.
#include "HwrMiscFlags.h"                  // hwrMiscFlagIDRocky
#undef NON_PORTABLE
#include "PalmPackPop.h"

// ---------------------------------------------------------------------------
//		� EmRegs328PalmPilot::GetLCDScreenOn
// ---------------------------------------------------------------------------

Bool EmRegs328PalmPilot::GetLCDScreenOn(void) {
    return (READ_REGISTER(portFData) & hwrTD1PortFLCDEnableOn) != 0;
}

// ---------------------------------------------------------------------------
//		� EmRegs328PalmPilot::GetLCDBacklightOn
// ---------------------------------------------------------------------------

Bool EmRegs328PalmPilot::GetLCDBacklightOn(void) {
    return (READ_REGISTER(portGData) & hwrTD1PortGBacklightOn) != 0;
}

// ---------------------------------------------------------------------------
//		� EmRegs328PalmPilot::GetLineDriverState
// ---------------------------------------------------------------------------
// Return whether or not the line drivers for the given object are open or
// closed.

Bool EmRegs328PalmPilot::GetLineDriverState(EmUARTDeviceType type) {
    if (type == kUARTSerial) return (READ_REGISTER(portGData) & hwrTD1PortGSerialOn) != 0;

    if (type == kUARTIR) return (READ_REGISTER(portJData) & hwrTD1PortJIrOn) == 0;

    return false;
}

// The Palm III (Rocky board ID). Other 328 boards are left as they were.
bool EmRegs328PalmPilot::RunsIdDetect() {
    if (fIdDetect < 0 && gSession)
        fIdDetect = gSession->GetDevice().HardwareID() == hwrMiscFlagIDRocky ? 1 : 0;
    return fIdDetect == 1;
}

namespace {
    // The Palm III's ADS7843: channel 2 is the battery. OS 3.0 has no sysBatteryData; its SysBatteryInfo takes
    // 78 steps per volt and reads its alkaline cells as 1.60 V empty to 3.00 V full, so the setting follows that line.
    class PalmIIIADC : public EmSPISlaveADS784x {
       public:
        PalmIIIADC()
            : EmSPISlaveADS784x(kChannelUnused, kChannelPenY, kChannelVirtualInput, kChannelUnused,
                                kChannelUnused, kChannelPenX, kChannelDockSerial, kChannelUnused) {}

        uint16 ReadVirtualInput(unsigned) const override {
            uint16 count;
            if (TableBatteryCount(false, count)) return count;
            const unsigned centivolts = 160 + 140 * gBattery.Percent() / 100;
            return ((centivolts * 78 + 50) / 100) << 4;
        }
    };
}  // namespace

EmRegs328PalmPilot::EmRegs328PalmPilot() : fSPISlaveADC(new PalmIIIADC()) {}

EmRegs328PalmPilot::~EmRegs328PalmPilot() = default;

void EmRegs328PalmPilot::Reset(Bool hardwareReset) {
    EmRegs328::Reset(hardwareReset);
    if (hardwareReset) fSPISlaveADC->Reset();
}

void EmRegs328PalmPilot::Save(Savestate<ChunkType>& savestate) {
    EmRegs328::Save(savestate);
    if (RunsIdDetect()) fSPISlaveADC->Save(savestate);
}

void EmRegs328PalmPilot::Save(SavestateProbe<ChunkType>& savestate) {
    EmRegs328::Save(savestate);
    if (RunsIdDetect()) fSPISlaveADC->Save(savestate);
}

void EmRegs328PalmPilot::Load(SavestateLoader<ChunkType>& loader) {
    EmRegs328::Load(loader);
    if (RunsIdDetect()) fSPISlaveADC->Load(loader);
}

// The Palm III has a Burr-Brown ADS7843 on SPIM, selected by PF7 low. The HAL reads the battery from it;
// pen positions come in through the pen patches. Other 328 boards keep the old pass-through.
EmSPISlave* EmRegs328PalmPilot::GetSPISlave() {
    if (RunsIdDetect() && (READ_REGISTER(portFData) & 0x80) == 0) return fSPISlaveADC.get();
    return nullptr;
}

// ---------------------------------------------------------------------------
//		� EmRegs328PalmPilot::GetPortInputValue
// ---------------------------------------------------------------------------
// Return the GPIO values for the pins on the port.  These values are used
// if the select pins are high.

uint8 EmRegs328PalmPilot::GetPortInputValue(int port) {
    uint8 result = EmRegs328::GetPortInputValue(port);

    if (port == 'M') {
        // Ensure that bit hwrTD1PortMDockIn is set.  If it's clear, HotSync
        // will sync via the modem instead of the serial port.

        result |= hwrTD1PortMDockIn;
    }

    // PE7 has a pull-up on the Palm III, so the ROM sees it high, runs ID DETECT and reads the Rocky ID
    // (IrDA and backlight). Low means a PalmPilot. Other 328 boards are left as they were.
    if (port == 'E' && RunsIdDetect()) result |= 0x80;

    return result;
}
