package com.rm.frond

import android.os.Build
import android.view.Choreographer
import android.view.KeyEvent
import android.view.MotionEvent

// Android's side of the core's input trace (input_trace.h, FROND_INPUT_TRACE from files/core-env on debug
// builds): each input event's own time and when the activity got it, and Choreographer's frame times. All
// times are CLOCK_MONOTONIC like the trace's host clock. UI thread only; off unless the trace is on.
object InputTrace {
    val on: Boolean by lazy { try { nativeOn() } catch (e: UnsatisfiedLinkError) { false } }

    private fun eventNanos(event: MotionEvent): Long =
        if (Build.VERSION.SDK_INT >= 34) event.eventTimeNanos else event.eventTime * 1_000_000L
    // KeyEvent has no public nanosecond time.
    private fun eventNanos(event: KeyEvent): Long = event.eventTime * 1_000_000L

    fun motion(kind: String, event: MotionEvent) {
        if (!on) return
        val action = event.actionMasked
        if (kind == "touch" && action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_UP &&
            action != MotionEvent.ACTION_MOVE && action != MotionEvent.ACTION_POINTER_DOWN &&
            action != MotionEvent.ACTION_POINTER_UP) return
        val oldest = if (event.historySize > 0) event.getHistoricalEventTime(0) * 1000L else eventNanos(event) / 1000L
        nativeEvent(kind, eventNanos(event) / 1000L, action, (event.getAxisValue(MotionEvent.AXIS_X) * 1000f).toInt(),
            (event.getAxisValue(MotionEvent.AXIS_Y) * 1000f).toInt(), event.historySize, oldest)
    }

    fun key(event: KeyEvent) {
        if (!on || event.repeatCount != 0) return
        nativeEvent("key", eventNanos(event) / 1000L, event.action, event.keyCode, event.deviceId, 0, eventNanos(event) / 1000L)
    }

    // Logs every Choreographer frame from now on, for lining events up with vsync.
    fun followFrames() {
        if (!on) return
        Choreographer.getInstance().postFrameCallback(object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                nativeFrame(frameTimeNanos / 1000L)
                Choreographer.getInstance().postFrameCallback(this)
            }
        })
    }

    @JvmStatic private external fun nativeOn(): Boolean
    @JvmStatic private external fun nativeEvent(kind: String, eventUsec: Long, action: Int, a: Int, b: Int, history: Int, oldestUsec: Long)
    @JvmStatic private external fun nativeFrame(frameUsec: Long)
}
