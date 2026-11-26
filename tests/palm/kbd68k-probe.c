/* Reads the Palm Portable Keyboard's serial stream as its driver would, through the Serial Manager
   that every 68k Palm OS has (SerOpen at 9600) and the UART's RTS bit, which it drives by hand:
   RTS low, wait for the keyboard's HotSync pulse, raise RTS and read the ID, read the keys typed,
   then RTS low again, wait for the pulse a key press gives, raise RTS and read the ID and the key
   still held, then its release. Each step goes on the screen and into the FrondKeyboardLog
   database, one record rewritten after every step, ending in "done" or the step that failed.
   The cradle key events are kept from the system, so the pulses don't start HotSync. */
#include <PalmOS.h>
#include <SerialMgrOld.h>

/* UMISC on the 68328 and 68EZ328: RTS follows the RTS bit when RTS_CONT is clear; the bit set
   drives the pin active. */
#define UMISC ((volatile UInt16*)0xfffff908)
#define RTS_CONT 0x0080
#define RTS_OUT 0x0040

static Int16 row = 2;
static Char log[700];
static UInt16 ref;

static void save(void) {
    LocalID id = DmFindDatabase(0, "FrondKeyboardLog");
    DmOpenRef db;
    MemHandle record;
    UInt16 at = 0;
    if (id) DmDeleteDatabase(0, id);
    if (DmCreateDatabase(0, "FrondKeyboardLog", 'FrKb', 'DATA', false)) return;
    id = DmFindDatabase(0, "FrondKeyboardLog");
    db = id ? DmOpenDatabase(0, id, dmModeReadWrite) : 0;
    if (!db) return;
    record = DmNewRecord(db, &at, StrLen(log) + 1);
    if (record) {
        DmWrite(MemHandleLock(record), 0, log, StrLen(log) + 1);
        MemHandleUnlock(record);
        DmReleaseRecord(db, at, true);
    }
    DmCloseDatabase(db);
}

static void show(const Char* text) {
    WinDrawChars(text, StrLen(text) > 38 ? 38 : StrLen(text), 2, row);
    row += 11;
    if (row > 150) row = 2;
    if (StrLen(log) + StrLen(text) + 2 < sizeof(log)) {
        StrCat(log, text);
        StrCat(log, "\n");
    }
    save();
}

static void rts(Boolean high) {
    UInt16 misc = *UMISC & ~(RTS_CONT | RTS_OUT);
    *UMISC = high ? misc | RTS_OUT : misc;
}

/* Waits up to ticks for a cradle key, keeping it from the system. Other events go on as usual. */
static Boolean waitPulse(Int32 ticks) {
    EventType e;
    UInt32 end = TimGetTicks() + ticks;
    while (TimGetTicks() < end) {
        EvtGetEvent(&e, 5);
        if (e.eType == keyDownEvent && e.data.keyDown.chr == vchrHardCradle) return true;
        if (e.eType == appStopEvent) return false;
        SysHandleEvent(&e);
    }
    return false;
}

/* Reads bytes as hex into line after label, until quiet ticks pass with none (after the first) or
   ticks in all. */
static void readBytes(const Char* label, Int32 quiet, Int32 ticks) {
    Char line[160];
    UInt8 buffer[16];
    UInt32 start = TimGetTicks(), last = 0, count, i;
    EventType e;
    Err error;
    StrCopy(line, label);
    while (TimGetTicks() - start < (UInt32)ticks) {
        if (last && TimGetTicks() - last > (UInt32)quiet) break;
        count = 0;
        error = SerReceiveCheck(ref, &count);
        if (error) SerClearErr(ref);
        if (count) {
            if (count > sizeof(buffer)) count = sizeof(buffer);
            count = SerReceive(ref, buffer, count, 0, &error);
            if (error) SerClearErr(ref);
            for (i = 0; i < count && StrLen(line) + 4 < sizeof(line); i++) {
                static const Char digits[] = "0123456789abcdef";
                Char hex[4];
                hex[0] = ' ';
                hex[1] = digits[buffer[i] >> 4];
                hex[2] = digits[buffer[i] & 15];
                hex[3] = 0;
                StrCat(line, hex);
            }
            last = TimGetTicks();
            continue;
        }
        EvtGetEvent(&e, 1);
        if (e.eType == keyDownEvent && e.data.keyDown.chr == vchrHardCradle) continue;
        SysHandleEvent(&e);
    }
    show(line);
}

static void run(void) {
    Err error;
    if (SysLibFind("Serial Library", &ref)) { show("no serial library"); return; }
    error = SerOpen(ref, 0, 9600);
    if (error && error != serErrAlreadyOpen) { show("open failed"); return; }
    rts(false);
    show("ready");
    if (!waitPulse(30 * SysTicksPerSecond())) { show("no pulse"); goto close; }
    show("pulse");
    rts(true);
    readBytes("id", SysTicksPerSecond() / 4, SysTicksPerSecond());
    readBytes("keys", 3 * SysTicksPerSecond(), 30 * SysTicksPerSecond());
    rts(false);
    show("low");
    if (!waitPulse(30 * SysTicksPerSecond())) { show("no second pulse"); goto close; }
    show("pulse again");
    rts(true);
    readBytes("wake", SysTicksPerSecond() / 2, SysTicksPerSecond());
    readBytes("up", 2 * SysTicksPerSecond(), 20 * SysTicksPerSecond());
    show("done");
close:
    rts(false);
    SerClose(ref);
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType e;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    WinEraseWindow();
    show("Frond keyboard probe");
    run();
    do {
        EvtGetEvent(&e, evtWaitForever);
        SysHandleEvent(&e);
    } while (e.eType != appStopEvent);
    return 0;
}
