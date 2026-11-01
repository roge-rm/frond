// FrondBench: times fixed amounts of integer, memory, drawing and database
// work so an emulated device can be compared with a real one.
// Needs Palm OS 3.5 or later. Results go on screen and in FrondBenchResults.
#include <PalmOS.h>
#include "frondbench.h"

#define appCreator 'FBnc'
#define resultsName "FrondBenchResults"
#define tempName "FrondBenchTemp"

// Work per test, sized for roughly 2 to 5 seconds on a Palm TX.
#define intLoops 3000000UL
#define memBlock 16384UL
#define memMoves 16000UL
#define drawLoops 8000UL
#define dbRounds 20
#define dbRecords 300
#define dbRecordSize 32

#define testCount 4
#define lineCount 10
#define lineLength 40
#define firstLineY 16

static const Char* testNames[testCount] = { "Integer", "Memory", "Drawing", "Database" };
static UInt32 testMs[testCount], testScore[testCount];
static Boolean haveResults;
static Char infoLine[2][lineLength];
static Char statusLine[lineLength];
static UInt32 checksum;
static UInt32 ticksPerSecond;

// Keeps the integer loop's result alive so the compiler can't drop the loop.
static volatile UInt32 sink;

static UInt32 ticksToMs(UInt32 ticks) {
    if (ticks < 0xFFFFFFFFUL / 1000) return ticks * 1000 / ticksPerSecond;
    return ticks / ticksPerSecond * 1000;
}

// Operations per second, without overflowing 32 bits.
static UInt32 perSecond(UInt32 ops, UInt32 ticks) {
    if (ticks == 0) ticks = 1;
    if (ops < 0xFFFFFFFFUL / ticksPerSecond) return ops * ticksPerSecond / ticks;
    return ops / ticks * ticksPerSecond;
}

static void formatId(Char* out, UInt32 id, Err error) {
    Int16 i;
    if (error) { StrCopy(out, "?"); return; }
    for (i = 0; i < 4; i++) {
        Char c = (Char)(id >> (24 - 8 * i));
        if (c < 32 || c > 126) { StrPrintF(out, "%lx", id); return; }
        out[i] = c;
    }
    out[4] = 0;
}

static void readDeviceInfo(void) {
    UInt32 rom = 0, company = 0, device = 0;
    Char companyText[12], deviceText[12];
    Err companyError, deviceError;
    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &rom);
    companyError = FtrGet(sysFtrCreator, sysFtrNumOEMCompanyID, &company);
    deviceError = FtrGet(sysFtrCreator, sysFtrNumOEMDeviceID, &device);
    formatId(companyText, company, companyError);
    formatId(deviceText, device, deviceError);
    StrPrintF(infoLine[0], "Palm OS %d.%d.%d  %lu ticks/s",
              sysGetROMVerMajor(rom), sysGetROMVerMinor(rom), sysGetROMVerFix(rom),
              ticksPerSecond);
    StrPrintF(infoLine[1], "Company %s  Device %s", companyText, deviceText);
}

static void drawLine(Int16 row, const Char* left, const Char* middle, const Char* right) {
    Coord y = firstLineY + row * FntLineHeight();
    RectangleType area;
    RctSetRectangle(&area, 0, y, 160, FntLineHeight());
    WinEraseRectangle(&area, 0);
    if (left) WinDrawChars(left, StrLen(left), 2, y);
    if (middle) WinDrawChars(middle, StrLen(middle), 88 - FntCharsWidth(middle, StrLen(middle)), y);
    if (right) WinDrawChars(right, StrLen(right), 158 - FntCharsWidth(right, StrLen(right)), y);
}

static void drawResults(void) {
    Int16 i;
    Char ms[16], score[16];
    UInt32 total = 0;
    FntSetFont(stdFont);
    drawLine(0, infoLine[0], 0, 0);
    drawLine(1, infoLine[1], 0, 0);
    drawLine(2, "Test", "ms", "per sec");
    for (i = 0; i < testCount; i++) {
        if (haveResults) {
            StrPrintF(ms, "%lu", testMs[i]);
            StrPrintF(score, "%lu", testScore[i]);
            total += testMs[i];
            drawLine(3 + i, testNames[i], ms, score);
        } else {
            drawLine(3 + i, testNames[i], "-", "-");
        }
    }
    if (haveResults) {
        StrPrintF(ms, "%lu", total);
        drawLine(7, "Total", ms, 0);
        StrPrintF(score, "%lx", checksum);
        drawLine(8, "Checksum", 0, score);
    } else {
        drawLine(7, "Total", "-", 0);
        drawLine(8, 0, 0, 0);
    }
    drawLine(9, statusLine, 0, 0);
}

static void setStatus(const Char* text) {
    StrCopy(statusLine, text);
    drawLine(9, statusLine, 0, 0);
}

// Integer math through PACE on OS 5: adds, 16-bit multiplies, shifts, xor
// and a small array. Returns the loop count it ran.
static UInt32 integerTest(void) {
    UInt32 table[16];
    UInt32 x = 1, y = 0x12345678UL, i;
    for (i = 0; i < 16; i++) table[i] = i * 2654435761UL;
    for (i = 0; i < intLoops; i++) {
        UInt16 slot = (UInt16)(i & 15);
        x += table[slot];
        x = (UInt32)(UInt16)x * 31421U + (x >> 16);
        y ^= x << 3;
        y = (y >> 5) | (y << 27);
        table[slot] = x ^ y;
    }
    sink = x ^ y;
    checksum = sink;
    return intLoops;
}

static UInt32 memoryTest(void) {
    MemPtr from = MemPtrNew(memBlock), to = MemPtrNew(memBlock);
    UInt32 i;
    if (!from || !to) {
        if (from) MemPtrFree(from);
        if (to) MemPtrFree(to);
        return 0;
    }
    MemSet(from, memBlock, 0x5A);
    for (i = 0; i < memMoves; i++) {
        if (i & 1) MemMove(from, to, memBlock);
        else MemMove(to, from, memBlock);
    }
    checksum += ((UInt8*)to)[memBlock - 1];
    MemPtrFree(from);
    MemPtrFree(to);
    return memMoves;
}

// Fills, erases and text on the results area, plus a 40x40 copy from an
// offscreen window each loop.
static UInt32 drawingTest(void) {
    Err error = 0;
    WinHandle screen = WinGetDrawWindow();
    WinHandle offscreen = WinCreateOffscreenWindow(40, 40, screenFormat, &error);
    RectangleType box, source;
    UInt32 i;
    const Char* text = "FrondBench 0123456789";
    if (!offscreen) return 0;
    RctSetRectangle(&source, 0, 0, 40, 40);
    WinSetDrawWindow(offscreen);
    WinEraseRectangle(&source, 0);
    RctSetRectangle(&box, 5, 5, 30, 30);
    WinDrawRectangle(&box, 4);
    WinDrawChars("FB", 2, 12, 14);
    WinSetDrawWindow(screen);
    for (i = 0; i < drawLoops; i++) {
        Coord x = (Coord)(i % 100), y = (Coord)(firstLineY + (i % 80));
        RctSetRectangle(&box, x, y, 60, 40);
        if (i & 1) WinEraseRectangle(&box, 0);
        else WinDrawRectangle(&box, 0);
        WinDrawChars(text, StrLen(text), (Coord)(i % 60), (Coord)(firstLineY + (i % 110)));
        WinCopyRectangle(offscreen, screen, &source, (Coord)(119 - (i % 120)),
                         (Coord)(firstLineY + (i % 90)), winPaint);
    }
    WinDeleteWindow(offscreen, false);
    return drawLoops;
}

// Creates a temporary database, writes and reads back small records, then
// deletes it, dbRounds times over.
static UInt32 databaseTest(void) {
    UInt8 data[dbRecordSize];
    UInt32 sum = 0;
    Int16 round, i;
    LocalID id = DmFindDatabase(0, tempName);
    if (id) DmDeleteDatabase(0, id);
    for (round = 0; round < dbRounds; round++) {
        DmOpenRef db;
        if (DmCreateDatabase(0, tempName, appCreator, 'TEMP', false)) return 0;
        id = DmFindDatabase(0, tempName);
        db = id ? DmOpenDatabase(0, id, dmModeReadWrite) : 0;
        if (!db) return 0;
        for (i = 0; i < dbRecords; i++) {
            UInt16 index = dmMaxRecordIndex;
            MemHandle record = DmNewRecord(db, &index, dbRecordSize);
            if (!record) break;
            MemSet(data, sizeof(data), (UInt8)(i + round));
            data[0] = (UInt8)i;
            DmWrite(MemHandleLock(record), 0, data, sizeof(data));
            MemHandleUnlock(record);
            DmReleaseRecord(db, index, true);
        }
        for (i = 0; i < (Int16)DmNumRecords(db); i++) {
            MemHandle record = DmQueryRecord(db, i);
            UInt8* bytes;
            Int16 b;
            if (!record) continue;
            bytes = MemHandleLock(record);
            for (b = 0; b < dbRecordSize; b++) sum += bytes[b];
            MemHandleUnlock(record);
        }
        DmCloseDatabase(db);
        DmDeleteDatabase(0, id);
    }
    checksum += sum;
    return (UInt32)dbRounds * dbRecords * 2;
}

// Replaces FrondBenchResults with one text record holding the last run.
static void saveResults(void) {
    Char text[400], line[60];
    LocalID id;
    DmOpenRef db;
    UInt16 index = 0;
    MemHandle record;
    UInt32 total = 0;
    Int16 i;
    StrPrintF(text, "FrondBench 1.0\n%s\n%s\n", infoLine[0], infoLine[1]);
    for (i = 0; i < testCount; i++) {
        StrPrintF(line, "%s %lu ms %lu/s\n", testNames[i], testMs[i], testScore[i]);
        StrCat(text, line);
        total += testMs[i];
    }
    StrPrintF(line, "Total %lu ms\nChecksum %lx\n", total, checksum);
    StrCat(text, line);
    id = DmFindDatabase(0, resultsName);
    if (id) DmDeleteDatabase(0, id);
    if (DmCreateDatabase(0, resultsName, appCreator, 'DATA', false)) return;
    id = DmFindDatabase(0, resultsName);
    db = id ? DmOpenDatabase(0, id, dmModeReadWrite) : 0;
    if (!db) return;
    record = DmNewRecord(db, &index, StrLen(text) + 1);
    if (record) {
        DmWrite(MemHandleLock(record), 0, text, StrLen(text) + 1);
        MemHandleUnlock(record);
        DmReleaseRecord(db, index, true);
    }
    DmCloseDatabase(db);
}

static void runTests(FormType* form) {
    Int16 i;
    Char status[lineLength];
    haveResults = false;
    checksum = 0;
    drawResults();
    for (i = 0; i < testCount; i++) {
        UInt32 start, ticks, ops = 0;
        StrPrintF(status, "Running %s...", testNames[i]);
        setStatus(status);
        EvtResetAutoOffTimer();
        start = TimGetTicks();
        switch (i) {
            case 0: ops = integerTest(); break;
            case 1: ops = memoryTest(); break;
            case 2: ops = drawingTest(); break;
            case 3: ops = databaseTest(); break;
        }
        ticks = TimGetTicks() - start;
        testMs[i] = ticksToMs(ticks);
        testScore[i] = perSecond(ops, ticks);
    }
    EvtResetAutoOffTimer();
    haveResults = true;
    saveResults();
    StrCopy(statusLine, "Done, results saved");
    FrmDrawForm(form);
    drawResults();
}

static Boolean mainFormHandleEvent(EventType* event) {
    FormType* form = FrmGetActiveForm();
    switch (event->eType) {
        case frmOpenEvent:
            FrmDrawForm(form);
            drawResults();
            return true;
        case frmUpdateEvent:
            FrmDrawForm(form);
            drawResults();
            return true;
        case ctlSelectEvent:
            if (event->data.ctlSelect.controlID == RunButton) {
                runTests(form);
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

static Boolean appHandleEvent(EventType* event) {
    if (event->eType == frmLoadEvent) {
        UInt16 formId = event->data.frmLoad.formID;
        FormType* form = FrmInitForm(formId);
        FrmSetActiveForm(form);
        if (formId == MainForm) FrmSetEventHandler(form, mainFormHandleEvent);
        return true;
    }
    return false;
}

static void eventLoop(void) {
    EventType event;
    Err error;
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (SysHandleEvent(&event)) continue;
        if (MenuHandleEvent(0, &event, &error)) continue;
        if (appHandleEvent(&event)) continue;
        FrmDispatchEvent(&event);
    } while (event.eType != appStopEvent);
}

UInt32 PilotMain(UInt16 cmd, MemPtr params, UInt16 flags) {
    UInt32 rom = 0;
    (void)params;
    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &rom);
    if (rom < sysMakeROMVersion(3, 5, 0, sysROMStageRelease, 0)) {
        if ((flags & sysAppLaunchFlagNewGlobals) && cmd == sysAppLaunchCmdNormalLaunch)
            FrmAlert(OldOSAlert);
        return sysErrRomIncompatible;
    }
    if (cmd != sysAppLaunchCmdNormalLaunch) return 0;
    ticksPerSecond = SysTicksPerSecond();
    if (ticksPerSecond == 0) ticksPerSecond = 100;
    readDeviceInfo();
    StrCopy(statusLine, "Tap Run to start");
    FrmGotoForm(MainForm);
    eventLoop();
    FrmCloseAllForms();
    return 0;
}
