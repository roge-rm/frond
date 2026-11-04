/* For the frond-68k sleep check: sets auto-off to 5 seconds and an alarm 20 seconds ahead. When
   the alarm goes off the probe plays a 1500 Hz tone, and when the system asks it to show the
   alarm it draws "alarm shown" and plays 1800 Hz. */
#include <PalmOS.h>

static void tone(Int32 frequency) {
    SndCommandType cmd;
    cmd.cmd = sndCmdFreqDurationAmp;
    cmd.param1 = frequency;
    cmd.param2 = 200;
    cmd.param3 = sndMaxAmp;
    SndDoCmd(NULL, &cmd, false);
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    UInt16 card;
    LocalID db;
    if (command == sysAppLaunchCmdAlarmTriggered) {
        tone(1500);
        return 0;
    }
    if (command == sysAppLaunchCmdDisplayAlarm) {
        WinDrawChars("alarm shown", 11, 2, 26);
        tone(1800);
        return 0;
    }
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    WinEraseWindow();
    WinDrawChars("Frond 68k alarm", 15, 2, 2);
    SysCurAppDatabase(&card, &db);
    SysSetAutoOffTime(5);
    AlmSetAlarm(card, db, 0, TimGetSeconds() + 20, true);
    WinDrawChars("alarm set", 9, 2, 14);
    do {
        EvtGetEvent(&event, evtWaitForever);
        SysHandleEvent(&event);
    } while (event.eType != appStopEvent);
    return 0;
}
