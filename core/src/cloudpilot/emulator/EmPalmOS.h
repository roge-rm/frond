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

#ifndef EmPalmOS_h
#define EmPalmOS_h

#include <functional>
#include <optional>

#include "EmCPU68K.h"  // ExceptionNumber
#include "EmPatchModuleTypes.h"
#include "EmStructs.h"  // EmStackFrameList
#include "EmThreadSafeQueue.h"
#include "KeyboardEvent.h"
#include "PenEvent.h"

class EmPalmOS {
   public:
    static void Initialize(void);
    static void Reset(void);
    static void Dispose(void);

    static void QueuePenEvent(PenEvent evt);
    static void QueueKeyboardEvent(KeyboardEvent evt);

    static bool HasPenEvent();
    static bool HasKeyboardEvent();
    static PenEvent PeekPenEvent();

    static void InjectSystemEvent(CallROMType& callROM);
    static void InjectUIEvent();

    static bool LaunchAppByName(const string& name);
    static bool HasPendingAppForLaunch();

    // Deletes a RAM database by name with DmDeleteDatabase. Calls into Palm OS, so only from a safe
    // point. A ROM database, the running application and an open database are refused.
    //
    // An application with the launcher in front answers pending: the launcher keeps its icons
    // locked and rebuilds its list when the database count changes, so the delete waits for the
    // launcher to quit. The launcher is relaunched, and the delete runs when the UI shell starts the
    // next application, with no application running. PendingDelete then has the result. Only one
    // waits at a time; it isn't saved in sessions and a reset drops it.
    enum class DeleteResult { deleted, notReady, missing, rom, running, open, failed, pending };
    static DeleteResult DeleteDatabaseByName(const string& name);
    // The result once a pending delete ran, or nullopt while it waits or when none is waiting.
    static std::optional<DeleteResult> TakeDeleteResult();
    static bool DeleteWaiting();
    static void CancelPendingDelete();
    // From the SysAppLaunch headpatch: a UI application is about to start.
    static void BeforeUIAppLaunch(LocalID next);

    // What Palm OS gets after a HotSync install, for a database just installed by name: the app
    // with its creator gets sysAppLaunchCmdSyncNotify unless it's the app in front, and an app
    // install while the launcher is in front relaunches the launcher so it lists the app. Any
    // other app in front stays. Calls into Palm OS, so only from a safe point. The pending
    // relaunch isn't saved in sessions.
    static void FinishInstall(const string& name);

    // Host work that calls into Palm OS runs at a safe point, where no app or system call is half
    // done: the start of EvtGetEvent, or its idle wait, outside any nested call. A request posts a
    // nil event so a dozing event loop goes round once; the handler runs once per request.
    static void SetSafePointHandler(std::function<void()> handler);
    static void RequestSafePoint();
    static void RunSafePoint();

   protected:
    static bool HandleTrap15(ExceptionNumber);
    static bool HandleJSR_Ind(emuptr oldPC, emuptr dest);

   private:
    static bool HandleSystemCall(Bool fromTrap);

    static bool DispatchNextEvent();
    static bool DispatchKeyboardEvent();
    static bool DispatchPenEvent();
    static void Wakeup();

    static void ClearQueues();

    static EmThreadSafeQueue<PenEvent> penEventQueue;
    static EmThreadSafeQueue<KeyboardEvent> keyboardEventQueue;

    static EmThreadSafeQueue<PenEvent> penEventQueueIncoming;
    static EmThreadSafeQueue<KeyboardEvent> keyboardEventQueueIncoming;
    static uint64 lastEventPromotedAt;

    static LocalID dbForLaunch;
    static LocalID pendingDelete;
    static std::optional<DeleteResult> deleteResult;
    static bool postNilEvent;

    static std::function<void()> safePointHandler;
    static bool safePointRequested;
};

#endif /* EmPalmOS_h */
