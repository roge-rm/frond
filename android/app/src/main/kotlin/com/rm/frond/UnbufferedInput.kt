package com.rm.frond

import android.os.Build
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View

// Touch moves and controller sticks go to the activity as soon as they come instead of being gathered up
// for the next frame, which held them back up to a frame before the core saw them. UI thread only.
object UnbufferedInput {
    // The source classes asked for at once; before Android 11 touches are asked for a gesture at a time.
    fun sources(sdk: Int): Int =
        if (sdk >= 30) InputDevice.SOURCE_CLASS_POINTER or InputDevice.SOURCE_CLASS_JOYSTICK else 0

    // The window's root view, so it holds whichever view has focus.
    fun request(root: View) {
        if (Build.VERSION.SDK_INT >= 30) root.requestUnbufferedDispatch(sources(Build.VERSION.SDK_INT))
    }

    // Each touch as it goes down, before Android 11.
    fun touch(root: View, event: MotionEvent) {
        if (Build.VERSION.SDK_INT < 30 && event.actionMasked == MotionEvent.ACTION_DOWN) root.requestUnbufferedDispatch(event)
    }
}
