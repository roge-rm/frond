/* For the tap checks: every pen down clears the screen and draws a 3x3 black square centred on
   where Palm OS put the tap, so a frame shows it. With the high-density Window Manager (version 4,
   Palm OS 5) the point and the square are in native pixels (EvtGetPenNative), else standard ones.
   On launch it makes FrondPenData, so it is an application with data and an icon for the delete
   checks. */
#include <PalmOS.h>

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    RectangleType mark;
    UInt32 version = 0;
    Boolean native, down;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    native = FtrGet(sysFtrCreator, sysFtrNumWinVersion, &version) == 0 && version >= 4;
    if (!DmFindDatabase(0, "FrondPenData")) DmCreateDatabase(0, "FrondPenData", 'FrPn', 'DATA', false);
    WinEraseWindow();
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (event.eType == penDownEvent && event.screenY < 160) {
            WinEraseWindow();
            mark.topLeft.x = event.screenX;
            mark.topLeft.y = event.screenY;
            if (native) {
                EvtGetPenNative(WinGetDisplayWindow(), &mark.topLeft.x, &mark.topLeft.y, &down);
                WinSetCoordinateSystem(kCoordinatesNative);
            }
            mark.topLeft.x -= 1;
            mark.topLeft.y -= 1;
            mark.extent.x = mark.extent.y = 3;
            WinDrawRectangle(&mark, 0);
            if (native) WinSetCoordinateSystem(kCoordinatesStandard);
            continue;
        }
        SysHandleEvent(&event);
    } while (event.eType != appStopEvent);
    return 0;
}
