/* Plays three tones through the Sound Manager for the frond-68k piezo check: 440, 1000 and
   2500 Hz for 300 ms each with 300 ms of silence between, then the Info system sound. */
#include <PalmOS.h>

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    static const Int32 tones[3] = {440, 1000, 2500};
    SndCommandType cmd;
    EventType event;
    int i;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    WinEraseWindow();
    WinDrawChars("Frond 68k tones", 15, 2, 2);
    SysTaskDelay(SysTicksPerSecond() / 2);
    for (i = 0; i < 3; i++) {
        cmd.cmd = sndCmdFreqDurationAmp;
        cmd.param1 = tones[i];
        cmd.param2 = 300;
        cmd.param3 = sndMaxAmp;
        SndDoCmd(NULL, &cmd, false);
        SysTaskDelay(SysTicksPerSecond() * 3 / 10);
    }
    SndPlaySystemSound(sndInfo);
    WinDrawChars("tones done", 10, 2, 14);
    do {
        EvtGetEvent(&event, evtWaitForever);
        SysHandleEvent(&event);
    } while (event.eType != appStopEvent && event.eType != penDownEvent);
    return 0;
}
