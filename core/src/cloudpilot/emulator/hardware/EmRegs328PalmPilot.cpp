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
#include "EmROMReader.h"
#include "EmSession.h"  // GetDevice, GetRomImage
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

// The Palm III on OS 3.x. OS 4.1's Rocky HAL reads the battery from the board, which isn't modelled,
// so with the Rocky ID it turns itself off at once; it stays on the PalmPilot path.
bool EmRegs328PalmPilot::RunsIdDetect() {
    if (fIdDetect < 0) {
        fIdDetect = 0;
        if (gSession && gSession->GetDevice().HardwareID() == hwrMiscFlagIDRocky) {
            const auto rom = gSession->GetRomImage();
            EmROMReader reader(rom.second, rom.first);
            int major = 0, minor, fix, stage, build;
            if (reader.Read()) reader.Version(major, minor, fix, stage, build);
            fIdDetect = major == 3;
        }
    }
    return fIdDetect == 1;
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
