#include <PalmOS.h>

/* The ARM callback owns this little-endian buffer until Delete returns.
 * Never call a database API from a callback or free state after failed Delete. */
typedef struct { UInt32 extended, seed, calls, overflow, events[256][6]; } AudioState;
static DmOpenRef logDB;
static MemHandle code;
static void* callback;
static UInt32 readLE(const void* ptr) {
    const UInt8* p = ptr;
    return (UInt32)p[0] | ((UInt32)p[1] << 8) | ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24);
}
static void writeLE(void* ptr, UInt32 value) {
    UInt8* p = ptr;
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}
static void record(const Char* text) {
    UInt16 index = dmMaxRecordIndex;
    MemHandle h = DmNewRecord(logDB, &index, StrLen(text) + 1);
    if (!h) return;
    DmWrite(MemHandleLock(h), 0, text, StrLen(text) + 1);
    MemHandleUnlock(h); DmReleaseRecord(logDB, index, true);
}
static void result(const Char* operation, Err error, UInt32 value) {
    Char text[120];
    StrPrintF(text, "%s err=%04x value=%lu tick=%lu", operation, error, value, TimGetTicks());
    record(text);
}
static void waitAudio(void) { SysTaskDelay(SysTicksPerSecond() / 4); }
static void dumpState(AudioState* state) {
    UInt32 count = readLE(&state->calls), i;
    Char text[180];
    result("callback-count", readLE(&state->overflow) ? 1 : 0, count);
    if (count > 256) count = 256;
    for (i = 0; i < count; ++i) {
        UInt32* e = state->events[i];
        StrPrintF(text, "block seed=%lu seq=%lu capacity=%lu actual=%lu checksum=%lu channel=%lu",
                  readLE(e+3), readLE(e), readLE(e+1), readLE(e+2), readLE(e+4), readLE(e+5));
        record(text);
    }
}
static Boolean runCase(Boolean extended, UInt32 seed) {
    SndStreamRef stream = 0;
    AudioState* state = MemPtrNew(sizeof(AudioState));
    Err error;
    Int32 value = 0;
    if (!state) { result("allocate", memErrNotEnoughSpace, seed); return true; }
    MemSet(state, sizeof(*state), 0);
    writeLE(&state->extended, extended); writeLE(&state->seed, seed);
    result("case", 0, seed);
    error = extended ? SndStreamCreateExtended(&stream, sndOutput, sndFormatPCM, 44100,
                sndInt16Little, sndStereo, (SndStreamVariableBufferCallback)callback, state, 4096, true)
        : SndStreamCreate(&stream, sndOutput, 44100, sndInt16Little, sndStereo,
                (SndStreamBufferCallback)callback, state, 1024, true);
    result(extended ? "create-extended" : "create-basic", error, stream);
    if (error) { MemPtrFree(state); return true; }
    result("volume-unity", SndStreamSetVolume(stream, 1024), seed);
    result("start", SndStreamStart(stream), seed); waitAudio();
    result("pause", SndStreamPause(stream, true), readLE(&state->calls));
    result("pause-return-calls", 0, readLE(&state->calls)); waitAudio();
    result("paused-calls", 0, readLE(&state->calls));
    result("resume", SndStreamPause(stream, false), seed); waitAudio();
    result("volume-half", SndStreamSetVolume(stream, 512), seed);
    error = SndStreamGetVolume(stream, &value); result("get-volume", error, (UInt32)value);
    waitAudio();
    result("pan-left", SndStreamSetPan(stream, sndPanFullLeft), seed);
    error = SndStreamGetPan(stream, &value); result("get-pan", error, (UInt32)value);
    waitAudio();
    result("pan-center", SndStreamSetPan(stream, sndPanCenter), seed);
    result("stop", SndStreamStop(stream), readLE(&state->calls));
    result("stop-return-calls", 0, readLE(&state->calls)); waitAudio();
    result("stopped-calls", 0, readLE(&state->calls));
    result("restart", SndStreamStart(stream), seed); waitAudio();
    error = SndStreamDelete(stream); result("delete", error, seed);
    if (error) return false; /* Resource remains locked; the callback may live. */
    dumpState(state);
    MemPtrFree(state);
    return true;
}
static Boolean runPair(void) {
    AudioState* states[2] = {0, 0};
    SndStreamRef streams[2] = {0, 0};
    UInt16 i;
    Boolean released = true;
    record("pair-begin");
    for (i = 0; i < 2; ++i) {
        Err error;
        states[i] = MemPtrNew(sizeof(AudioState));
        if (!states[i]) { result("pair-allocation", 1, i); break; }
        MemSet(states[i], sizeof(AudioState), 0);
        writeLE(&states[i]->seed, 4 + i);
        error = SndStreamCreate(&streams[i], sndOutput, 44100, sndInt16Little, sndStereo,
                    (SndStreamBufferCallback)callback, states[i], 1024, true);
        result("pair-create", error, i);
        if (error) { streams[i] = 0; break; }
        result("pair-start", SndStreamStart(streams[i]), i);
    }
    waitAudio(); waitAudio();
    for (i = 0; i < 2; ++i) {
        Err error = streams[i] ? SndStreamDelete(streams[i]) : 0;
        result("pair-delete", error, i);
        if (error) released = false;
        else if (states[i]) { dumpState(states[i]); MemPtrFree(states[i]); }
    }
    record("pair-end");
    return released;
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Err menuError;
    LocalID id;
    Boolean released = true;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    id = DmFindDatabase(0, "FrondAudioLog");
    if (id) DmDeleteDatabase(0, id);
    if (DmCreateDatabase(0, "FrondAudioLog", 'FrAu', 'DATA', false)) return 1;
    logDB = DmOpenDatabase(0, DmFindDatabase(0, "FrondAudioLog"), dmModeReadWrite);
    if (!logDB) return 1;
    code = DmGetResource('armc', 1000);
    callback = code ? MemHandleLock(code) : 0;
    record("audio-probe-v1 signed16le stereo44100 full-half-zero-full");
    EvtSetAutoOffTimer(SetAtLeast, 300);
    if (callback) {
        released = runCase(false, 1);
        if (released) released = runCase(true, 2);
        if (released) released = runCase(false, 3); /* Delete/reopen address reuse. */
        if (released) released = runPair();
    } else result("callback-resource", 1, 0);
    if (released && code) { MemHandleUnlock(code); DmReleaseResource(code); }
    record(released ? "complete" : "incomplete-delete-failed");
    DmCloseDatabase(logDB);
    EvtResetAutoOffTimer();
    FrmGotoForm(1000);
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent) {
                FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
                FrmDrawForm(FrmGetActiveForm());
            } else FrmHandleEvent(FrmGetActiveForm(), &event);
        }
    } while (event.eType != appStopEvent);
    FrmCloseAllForms();
    return 0;
}
