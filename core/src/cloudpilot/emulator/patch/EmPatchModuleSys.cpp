#include "EmPatchModuleSys.h"

#include <memory>

#include "EmBattery.h"
#include "EmCPU.h"
#include "EmCommon.h"
#include "EmLowMem.h"
#include "EmPalmOS.h"
#include "EmPalmStructs.h"
#include "EmPatchMgr.h"
#include "EmSession.h"
#include "EmSystemState.h"
#include "EmRegsFMSound.h"
#include "Feature.h"
#include "Logging.h"
#include "Marshal.h"
#include "Miscellaneous.h"
#include "NetworkProxy.h"
#include "PenEvent.h"
#include "Platform.h"
#include "ROMStubs.h"
#include "SuspendContextClipboardCopy.h"
#include "SuspendContextClipboardPaste.h"
#include "SuspendManager.h"
#include "encoding.h"

// #define LOGGING
#ifdef LOGGING
    #define PRINTF logging::printf
#else
    #define PRINTF(...) ;
#endif

namespace {
    CallROMType HeadpatchDmInit(void) {
        gSession->ReleaseBootKeys();

        PRINTF("syscall: DmInit");

        return kExecuteROM;
    }

    CallROMType HeadpatchSysEvGroupWait(void) {
        // Only do this under 2.0 and later.  Under Palm OS 1.0, EvtGetSysEvent
        // called SysSemaphoreWait instead.  See our headpatch of that function
        // for a chunk of pretty similar code.

        if (gSystemState.OSMajorVersion() == 1) {
            return kExecuteROM;
        }

        // Err SysEvGroupWait(UInt32 evID, UInt32 mask, UInt32 value, Int32 matchType,
        //						 Int32 timeout)

        CALLED_SETUP("Err",
                     "UInt32 evID, UInt32 mask, UInt32 value, Int32 matchType,"
                     "Int32 timeout");

        CALLED_GET_PARAM_VAL(Int32, timeout);

        CallROMType result = kExecuteROM;

        EmPalmOS::InjectSystemEvent(result);

        return result;
    }

    CallROMType HeadpatchSysSemaphoreWait(void) {
        // Only do this under 1.0.	Under Palm OS 2.0 and later, EvtGetSysEvent
        // calls SysEvGroupWait instead.  See our headpatch of that function
        // for a chunk of pretty similar code.

        if (gSystemState.OSMajorVersion() != 1) {
            return kExecuteROM;
        }

        // Err SysSemaphoreWait(UInt32 smID, UInt32 priority, Int32 timeout)

        CALLED_SETUP("Err", "UInt32 smID, UInt32 priority, Int32 timeout");

        CALLED_GET_PARAM_VAL(Int32, timeout);

        CallROMType result;

        EmPalmOS::InjectSystemEvent(result);

        return result;
    }

    CallROMType HeadpatchPenScreenToRaw_RawToScreen(void) {
        EmAssert(gSession);
        if (gSession->GetDevice().HasCustomDigitizerTransform()) return kExecuteROM;

        CALLED_SETUP("Err", "PointType* point");

        CALLED_GET_PARAM_REF(PointType, point, Marshal::kInOut);

        TransformPenCoordinates((*point).x, (*point).y);

        CALLED_PUT_PARAM_REF(point);
        PUT_RESULT_VAL(Err, 0);

        return kSkipROM;
    }

    CallROMType HeadpatchPenCalibrate(void) {
        EmAssert(gSession);

        if (gSession->GetDevice().HasCustomDigitizerTransform()) {
            InstallCalibrationInfo();

            CALLED_SETUP("Err", "void");
            PUT_RESULT_VAL(Err, 0);

            return kSkipROM;
        }

        return kExecuteROM;
    }

    CallROMType HeadpatchSysUIAppSwitch() {
        gSystemState.SetSetupComplete();

        return kExecuteROM;
    }

    CallROMType HeadpatchHwrDockStatus(void) {
        // On non-328 devices, we emulate the hardware that returns this
        // information, so we don't need to patch this function.

        EmAssert(gSession);

        if (gSession->GetDevice().EmulatesDockStatus()) {
            return kExecuteROM;
        }

        // hwrDockStatusState HwrDockStatus(void)
        //
        //	(added in Palm OS 3.1)
        //	(changed later to return UInt16)

        CALLED_SETUP("UInt16", "void");

        // Old enumerated values from Hardware.h:
        //
        //		DockStatusNotDocked = 0,
        //		DockStatusInModem,
        //		DockStatusInCharger,
        //		DockStatusUnknown = 0xFF

        // New defines from HwrDock.h
#define hwrDockStatusUndocked 0x0000            // nothing is attached
#define hwrDockStatusModemAttached 0x0001       // some type of modem is attached
#define hwrDockStatusDockAttached 0x0002        // some type of dock is attached
#define hwrDockStatusUsingExternalPower 0x0004  // using some type of external power source
#define hwrDockStatusCharging 0x0008            // internal power cells are recharging

        // On the charger (the battery setting's Charging switch), charging until full, or on battery with nothing
        // attached.
        PUT_RESULT_VAL(UInt16, gBattery.charging
                                   ? UInt16(hwrDockStatusUsingExternalPower |
                                            (gBattery.ChargeInProgress() ? hwrDockStatusCharging : 0))
                                   : UInt16(hwrDockStatusUndocked));

        return kSkipROM;
    }

    CallROMType HeadpatchClipboardGetItem() {
        EmAssert(gSession);

        if (Feature::GetClipboardIntegration() && !gSession->IsNested()) {
            SuspendManager::Suspend<SuspendContextClipboardPaste>();
        }

        return kExecuteROM;
    }

    CallROMType HeadpatchDbgMessage() {
        CALLED_SETUP("void", "Char* aStr");

        CALLED_GET_PARAM_STR(Char, aStr);

        cout << "DbgMessage: " << static_cast<char*>(aStr) << endl << flush;

        return kSkipROM;
    }

    // The battery setting's level as the 0-255 scale HwrBatteryLevel and HwrBattery's abs use.
    UInt16 BatteryLevel255() { return UInt16(gBattery.Percent() * 255 / 100); }

    // The battery voltage for the setting's level: the ROM's sysBattVoltageCurve scaled so full is fullMilliVolts,
    // or 80% of full at empty rising evenly where the ROM has no curve.
    UInt16 BatteryMilliVolts(UInt16 fullMilliVolts) {
        const unsigned percent = gBattery.Percent();
        if (percent == 100) return fullMilliVolts;

        CEnableFullAccess munge;
        const emuptr data = EmLowMem_GetGlobal(sysBatteryDataP);
        if (data) {
            EmAliasSysBatteryDataStructV1<PAS> v1(data);
            const UInt16 version = v1.sysBattDataStructVersion;
            uint16_t curve[11] = {};
            for (int i = 0; i < 11; i++) {
                if (version == 1) curve[i] = v1.sysBattVoltageCurve[i];
                else if (version == 2) curve[i] = EmAliasSysBatteryDataStructV2<PAS>(data).sysBattVoltageCurve[i];
                else if (version == 3) curve[i] = EmAliasSysBatteryDataStructV3<PAS>(data).sysBattVoltageCurve[i];
            }
            if (curve[10])
                return UInt16(uint32_t(fullMilliVolts) * BatteryPower::CurveCentivolts(curve, percent) / curve[10]);
        }
        return UInt16(uint32_t(fullMilliVolts) * (400 + percent) / 500);
    }

    // The ROM's sysBatteryMinThreshold as SysBattery sets it from its battery data: the shutdown voltage in level
    // units, centivolts * steps per volt / 100 less the step offset. 0 when the ROM has no battery data.
    UInt8 RomBatteryMinThreshold() {
        CEnableFullAccess munge;
        const emuptr data = EmLowMem_GetGlobal(sysBatteryDataP);
        if (!data) return 0;
        int volts = 0, steps = 0, offset = 0;
        EmAliasSysBatteryDataStructV1<PAS> v1(data);
        if (v1.sysBattDataStructVersion == 1) {
            volts = v1.sysBattShutdownEnVoltage; steps = v1.sysBattStepsPerVolt; offset = v1.sysBattVoltageStepOffset;
        } else if (v1.sysBattDataStructVersion == 2) {
            EmAliasSysBatteryDataStructV2<PAS> v2(data);
            volts = v2.sysBattShutdownEnVoltage; steps = v2.sysBattStepsPerVolt; offset = v2.sysBattVoltageStepOffset;
        } else if (v1.sysBattDataStructVersion == 3) {
            EmAliasSysBatteryDataStructV3<PAS> v3(data);
            volts = v3.sysBattShutdownEnVoltage; steps = v3.sysBattStepsPerVolt; offset = v3.sysBattVoltageStepOffset;
        } else {
            return 0;
        }
        const int level = (volts * steps + 50) / 100 - offset;
        return UInt8(level < 1 ? 1 : level > 255 ? 255 : level);
    }

    CallROMType HeadpatchHwrBatteryLevel(void) {
        EmAssert(gSession);

        if (!gSession->GetDevice().NeedsBatteryPatch()) {
            return kExecuteROM;
        }

        CALLED_SETUP("UInt16", "void");

        PUT_RESULT_VAL(UInt16, BatteryLevel255());

        return kSkipROM;
    }

    CallROMType HeadpatchHwrBattery(void) {
        EmAssert(gSession);

        CALLED_SETUP("Err", "UInt16 cmd, void * cmdP");
        CALLED_GET_PARAM_VAL(UInt16, cmd);

        // hwrBatteryCmdMainRead
        if (cmd == 2 && gSession->GetDevice().NeedsBatteryPatch()) {
            CALLED_GET_PARAM_REF(HwrBatCmdReadType, cmdP, Marshal::kInOut);

            // Full is 4.0 V on the Prism and 2.5 V elsewhere.
            const bool prism = gSession->GetDevice().HardwareID() == 0x0a /*halModelIDVisorPrism*/;
            (*cmdP).mVolts = BatteryMilliVolts(prism ? 4000 : 2500);
            (*cmdP).abs = BatteryLevel255();

            cmdP.Put();

            PUT_RESULT_VAL(Err, errNone);
            return kSkipROM;
        }

        return kExecuteROM;
    }

    void TailpatchHwrBattery(void) {
        if (!gSession->GetDevice().NeedsBatteryGlobalsHack()) return;

        CEnableFullAccess munge;

        EmLowMem::fgLowMem.globals.hwrBatteryLevel = BatteryLevel255();
        EmLowMem::fgLowMem.globals.hwrBatteryPercent = gBattery.Percent();
        // Full clears the shutdown threshold. Below full a cleared one gets the ROM's value back, so a low level
        // warns and turns the Palm off.
        if (gBattery.Percent() == 100) EmLowMem::fgLowMem.globals.sysBatteryMinThreshold = 0;
        else if (!EmLowMem::fgLowMem.globals.sysBatteryMinThreshold)
            EmLowMem::fgLowMem.globals.sysBatteryMinThreshold = RomBatteryMinThreshold();
    }

    // The m520 can fill its USB receive queue before the ROM finishes reading a HotSync write.
    constexpr UInt16 m520StockUsbReceiveSize = 1062;
    constexpr UInt16 m520UsbReceiveSize = 8 * 1024;

    bool IsM520UsbReceiveSetup(emuptr trapWord) {
        return gCPU->GetPC() == trapWord && gSession->GetDevice().GetIDString() == "PalmM520" &&
               EmMemGet32(0x100ab85c) == 0x48780426 && EmMemGet32(0x100ab8fc) == 0x3f3c0426 &&
               EmMemGet32(0x100ab8d2) == 0x73796e63 && EmMemGet32(0x100ab908) == 0x74134e4f;
    }

    CallROMType HeadpatchM520UsbReceiveAllocate() {
        if (IsM520UsbReceiveSetup(0x100ab862)) {
            const emuptr size = gCPU->GetSP();
            if (EmMemGet32(size) == m520StockUsbReceiveSize) EmMemPut32(size, m520UsbReceiveSize);
        }
        return kExecuteROM;
    }

    CallROMType HeadpatchM520UsbReceiveBuffer() {
        if (!IsM520UsbReceiveSetup(0x100ab90c)) return kExecuteROM;

        // SrmSetReceiveBuffer takes a port ID, a buffer pointer and its size.
        const emuptr args = gCPU->GetSP();
        const emuptr buffer = EmMemGet32(args + 2);
        if (buffer && EmMemGet16(args + 6) == m520StockUsbReceiveSize && MemPtrSize(buffer) >= m520UsbReceiveSize)
            EmMemPut16(args + 6, m520UsbReceiveSize);
        return kExecuteROM;
    }

    CallROMType HeadpatchSysLibFind() {
        constexpr emuptr unregisterFind = 0x102d361a;
        // The stock m520's Bluetooth Exchange callback runs with HotSync's globals.
        // Skip that callback's unload of the unsupported Bluetooth library.
        if (gCPU->GetPC() != unregisterFind ||
            gSession->GetDevice().GetIDString() != "PalmM520" ||
            EmMemGet32(0x102d3640) != 0x2f280594)
            return kExecuteROM;

        CALLED_SETUP("Err", "const Char* nameP, UInt16* refNumP");
        CALLED_GET_PARAM_VAL(emuptr, nameP);
        constexpr char libraryName[] = "Bluetooth Exchange Library";
        if (nameP != 0x102d3674 ||
            EmMem_memcmp(static_cast<emuptr>(nameP), static_cast<const void*>(libraryName), sizeof(libraryName)) != 0)
            return kExecuteROM;

        PUT_RESULT_VAL(Err, sysErrLibNotFound);
        return kSkipROM;
    }

    CallROMType HeadpatchSysLibLoad(void) {
        CALLED_SETUP("Err", "UInt32 libType, UInt32 libCreator, UInt16* refNumP");

        CALLED_GET_PARAM_VAL(UInt32, libType);
        CALLED_GET_PARAM_VAL(UInt32, libCreator);

        if (gSession->GetDevice().IsClie() && libType == sysResTLibrary &&
            (libCreator == 'SlMa' || libCreator == 'yP1L')) {
            if (libCreator == 'yP1L' && EmRegsFMSound::Enabled()) return kExecuteROM;
            PUT_RESULT_VAL(Err, sysErrLibNotFound);
            return kSkipROM;
        }

        return kExecuteROM;
    }

    void TailpatchFtrInit(void) {
        PRINTF("syscall: FtrInit");

        uint32 value;

        Err err = ::FtrGet(sysFtrCreator, sysFtrNumROMVersion, &value);

        if (err == errNone) {
            gSystemState.SetOSVersion(value);

            PRINTF("PalmOS version: %u.%u", gSystemState.OSMajorVersion(),
                   gSystemState.OSMinorVersion());
        } else {
            // EmSystemState::SetOSVersion(kOSUndeterminedVersion);
            PRINTF("vailed to determine PalmOS version");
        }
    }

    void TailpatchHwrMemReadable(void) {
        CALLED_SETUP("UInt32", "MemPtr address");

        CALLED_GET_PARAM_VAL(emuptr, address);
        GET_RESULT_VAL(uint32);

        if (result == 0) {
            void* addrStart;
            uint32 addrLen;

            Memory::GetMappingInfo(address, &addrStart, &addrLen);

            PUT_RESULT_VAL(uint32, addrLen);
        }

        PRINTF("syscall: HwrMemReadable for 0x%08x", address);
    }

    void TailpatchTimInit(void) {
        // Turn off the RTC bug workaround flag.

        CEnableFullAccess munge;

        emuptr timGlobalsP = EmLowMem_GetGlobal(timGlobalsP);
        EmAliasTimGlobalsType<PAS> timGlobals(timGlobalsP);

        if (timGlobals.rtcBugWorkaround == 0x01) {
            timGlobals.rtcBugWorkaround = 0;
        }

        // Set the current date.

        gSystemState.SetTimeInitialized();
        SetCurrentDate();

        PRINTF("syscall: TimInit");
    }

    void TailpatchUIInitialize(void) {
        // Auto-off stays as the OS set it from the Prefs, as on the real Palm.
        EmLowMem::fgLowMem.globals.sysRandomSeed = Platform::Random();

        ::FtrSet('pose', 0, 0);
        ::FtrSet('cldp', 0, 0x20150103);

#if 0  // CSTODO
       // Can't call PrefSetPreference on 1.0 devices....
        if (EmLowMem::TrapExists(sysTrapPrefSetPreference)) {
            ::PrefSetPreference(prefAutoOffDuration, 0);
        }
#endif

        SetHotSyncUserName(gSystemState.GetHotsyncUserName().c_str());

        gSystemState.SetUIInitialized();

        gNetworkProxy.Reset();

        PRINTF("syscall: UIInitialize");
    }

    void TailpatchEvtSysEventAvail() {
        // Boolean EvtSysEventAvail(Boolean ignorePenUps)

        CALLED_SETUP("Boolean", "Boolean ignorePenUps");

        CALLED_GET_PARAM_VAL(Boolean, ignorePenUps);
        GET_RESULT_VAL(Boolean);

        if (result == 0) {
            bool handled = false;

            if (EmPalmOS::HasPenEvent()) {
                PenEvent event = EmPalmOS::PeekPenEvent();

                if (event.isPenDown() || !ignorePenUps) {
                    PUT_RESULT_VAL(Boolean, true);
                    handled = true;
                }
            }

            // This is a nasty hack in order to make sure that launching apps works if the Pilot
            // is currently executing an app that does no run the event loop until EvtSysEventAvail
            // returns true.
            if (!handled && EmPalmOS::HasPendingAppForLaunch()) {
                PUT_RESULT_VAL(Boolean, true);
                handled = true;
            }
        }
    }

    void DispatchCopyClipboard() {
        UInt16 length;
        emuptr dataHdl = ClipboardGetItem(clipboardText, &length);

        if (length > 0) {
            emuptr dataPtr = MemHandleLock(dataHdl);

            if (dataPtr) {
                unique_ptr<char[]> dataCopy = make_unique<char[]>(length + 1);
                dataCopy[length] = 0;

                EmMem_memcpy(static_cast<void*>(dataCopy.get()), dataPtr, length);

                MemHandleUnlock(dataHdl);

                SuspendManager::Suspend<SuspendContextClipboardCopy>(
                    Isolatin1ToUtf8(dataCopy.get()));
            }
        } else {
            SuspendManager::Suspend<SuspendContextClipboardCopy>("");
        }
    }

    void TailpatchClipboardAddItem() {
        EmAssert(gSession);

        if (Feature::GetClipboardIntegration() && !gSession->IsNested()) DispatchCopyClipboard();
    }

    void TailpatchClipboardAppendItem() {
        EmAssert(gSession);

        if (Feature::GetClipboardIntegration() && !gSession->IsNested()) DispatchCopyClipboard();
    }

    CallROMType HeadpatchHwrIRQ4Handler() {
        // PalmOS will increment the date by one day if this is a rollover interrupt,
        // so make sure that the day is set in one day in the past. This should
        // make sure that PalmOS calculates programs the alarms correctly.
        SetCurrentDate(-1);

        return kExecuteROM;
    }

    void TailpatchHwrIRQ4Handler() {
        // If this wasn't a rollover interrupt we're gonne be one day in the past by now, so
        // make sure that the date is correct again.
        SetCurrentDate();
    }

    CallROMType HeadpatchSysSleep() {
        if (!gSession->GetDevice().BuggySleep()) return kExecuteROM;

        CALLED_SETUP("Err", "void");

        PUT_RESULT_VAL(Err, 0);

        return kSkipROM;
    }

    // The UI shell starts the next UI application here, after the last one quit: a delete that
    // waits for the launcher to let go runs now.
    CallROMType HeadpatchSysAppLaunch() {
        if (!EmPalmOS::DeleteWaiting()) return kExecuteROM;

        CALLED_SETUP("Err",
                     "UInt16 cardNo, LocalID dbID, UInt16 launchFlags, UInt16 cmd, MemPtr cmdPBP,"
                     "UInt32* resultP");

        CALLED_GET_PARAM_VAL(LocalID, dbID);
        CALLED_GET_PARAM_VAL(UInt16, launchFlags);

        if (launchFlags & sysAppLaunchFlagUIApp)
            EmPalmOS::BeforeUIAppLaunch(dbID);

        return kExecuteROM;
    }

    CallROMType HeadpatchEvtGetEvent() {
        EmPalmOS::RunSafePoint();
        EmPalmOS::InjectUIEvent();

        return kExecuteROM;
    }

    ProtoPatchTableEntry protoPatchTable[] = {
        {sysTrapDmInit, HeadpatchDmInit, NULL},
        {sysTrapSysUIAppSwitch, HeadpatchSysUIAppSwitch, NULL},
        {sysTrapSysEvGroupWait, HeadpatchSysEvGroupWait, NULL},
        {sysTrapFtrInit, NULL, TailpatchFtrInit},
        {sysTrapHwrMemReadable, NULL, TailpatchHwrMemReadable},
        {sysTrapTimInit, NULL, TailpatchTimInit},
        {sysTrapUIInitialize, NULL, TailpatchUIInitialize},
        {sysTrapEvtSysEventAvail, NULL, TailpatchEvtSysEventAvail},
        {sysTrapHwrDockStatus, HeadpatchHwrDockStatus, NULL},
        {sysTrapPenScreenToRaw, HeadpatchPenScreenToRaw_RawToScreen, NULL},
        {sysTrapPenRawToScreen, HeadpatchPenScreenToRaw_RawToScreen, NULL},
        {sysTrapPenCalibrate, HeadpatchPenCalibrate, NULL},
        {sysTrapClipboardGetItem, HeadpatchClipboardGetItem, NULL},
        {sysTrapClipboardAddItem, NULL, TailpatchClipboardAddItem},
        {sysTrapClipboardAppendItem, NULL, TailpatchClipboardAppendItem},
        {sysTrapSysSemaphoreWait, HeadpatchSysSemaphoreWait, NULL},
        {sysTrapDbgMessage, HeadpatchDbgMessage, NULL},
        {sysTrapHwrBatteryLevel, HeadpatchHwrBatteryLevel, NULL},
        {sysTrapHwrBattery, HeadpatchHwrBattery, TailpatchHwrBattery},
        {sysTrapSysLibLoad, HeadpatchSysLibLoad, NULL},
        {sysTrapSysLibFind, HeadpatchSysLibFind, NULL},
        {sysTrapMemPtrNew, HeadpatchM520UsbReceiveAllocate, NULL},
        {sysTrapSerialDispatch, HeadpatchM520UsbReceiveBuffer, NULL},
        {sysTrapHwrIRQ4Handler, HeadpatchHwrIRQ4Handler, TailpatchHwrIRQ4Handler},
        {sysTrapSysSleep, HeadpatchSysSleep, NULL},
        {sysTrapEvtGetEvent, HeadpatchEvtGetEvent, NULL},
        {sysTrapSysAppLaunch, HeadpatchSysAppLaunch, NULL},
        {0, NULL, NULL}};
}  // namespace

EmPatchModuleSys::EmPatchModuleSys() { LoadProtoPatchTable(protoPatchTable); }
