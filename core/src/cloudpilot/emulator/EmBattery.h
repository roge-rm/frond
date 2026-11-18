#ifndef EmBattery_h
#define EmBattery_h

#include "BatteryPower.h"

// The 68k machine's battery setting. Read by the HwrBattery and HwrDockStatus patches and the boards'
// battery and power inputs; set by the frontend (--battery, the battery command). Not saved with the session.
inline BatteryPower gBattery;

#endif
