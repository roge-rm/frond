package com.rm.frond

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.util.Xml
import android.view.accessibility.AccessibilityNodeInfo
import java.io.StringWriter
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONObject
import java.io.File

// Test-only event injection through Android's input dispatcher, including simultaneous touches.
internal class InputAcceptance(private val runner: Instrumentation) {
    private val context get() = runner.targetContext
    private var touchStart = 0L

    fun run() {
        val folder = File(context.filesDir, "acceptance-input").apply { mkdirs() }
        runner.startActivitySync(Intent(context, HomeActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        runner.sendStatus(0, Bundle().apply { putString("stream", "Input acceptance ready\n") })
        val end = SystemClock.uptimeMillis() + 30 * 60 * 1000
        while (SystemClock.uptimeMillis() < end) {
            val next = folder.listFiles()?.filter { it.name.endsWith(".json") && !it.name.endsWith(".done.json") }
                ?.sortedBy { it.name }?.firstOrNull { !File(folder, it.nameWithoutExtension + ".done.json").exists() }
            if (next == null) { SystemClock.sleep(100); continue }
            val plan = JSONObject(next.readText())
            var quit = false
            val result = runCatching {
                val events = plan.getJSONArray("events")
                for (i in 0 until events.length()) {
                    val event = events.getJSONObject(i)
                    when (event.getString("type")) {
                        "key" -> key(event)
                        "touch" -> touch(event)
                        "wait" -> SystemClock.sleep(event.getLong("ms").coerceIn(0, 5000))
                        "dump" -> File(folder, "window.xml").writeText(windowXml())
                        "screenshot" -> {
                            val name = event.getString("name")
                            check(name.matches(Regex("[A-Za-z0-9_-]+")))
                            File(folder, "$name.png").outputStream().use {
                                check(runner.uiAutomation.takeScreenshot().compress(Bitmap.CompressFormat.PNG, 100, it))
                            }
                        }
                        "quit" -> quit = true
                        else -> error("Unknown input event")
                    }
                }
            }
            File(folder, next.nameWithoutExtension + ".done.json").writeText(
                JSONObject().put("ok", result.isSuccess).put("error", result.exceptionOrNull()?.stackTraceToString()).toString())
            if (quit) {
                runner.finish(Activity.RESULT_OK, Bundle().apply { putString("stream", "Input acceptance stopped\n") })
                return
            }
        }
        runner.finish(Activity.RESULT_CANCELED, Bundle().apply { putString("stream", "Input acceptance timed out\n") })
    }

    private fun windowXml(): String {
        val text = StringWriter()
        val xml = Xml.newSerializer().apply { setOutput(text); startDocument("UTF-8", true); startTag(null, "hierarchy") }
        fun visit(node: AccessibilityNodeInfo) {
            xml.startTag(null, "node")
            val bounds = Rect().also { node.getBoundsInScreen(it) }
            val attrs = mapOf("text" to (node.text?.toString() ?: ""),
                "content-desc" to (node.contentDescription?.toString() ?: ""),
                "class" to (node.className?.toString() ?: ""), "bounds" to bounds.toShortString(),
                "enabled" to node.isEnabled.toString(), "checkable" to node.isCheckable.toString(),
                "checked" to node.isChecked.toString(), "clickable" to node.isClickable.toString())
            attrs.forEach { (key, value) -> xml.attribute(null, key, value) }
            for (i in 0 until node.childCount) node.getChild(i)?.let { visit(it) }
            xml.endTag(null, "node")
        }
        visit(checkNotNull(runner.uiAutomation.rootInActiveWindow))
        xml.endTag(null, "hierarchy"); xml.endDocument()
        return text.toString()
    }

    private fun key(event: JSONObject) {
        val now = SystemClock.uptimeMillis()
        val source = if (event.optString("source") == "gamepad") InputDevice.SOURCE_GAMEPAD else InputDevice.SOURCE_KEYBOARD
        val key = KeyEvent(now, now, if (event.getBoolean("down")) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP,
            event.getInt("code"), 0, event.optInt("meta"), -1, event.optInt("scan"), 0, source)
        check(runner.uiAutomation.injectInputEvent(key, true))
    }

    private fun touch(event: JSONObject) {
        val action = event.getInt("action")
        val now = SystemClock.uptimeMillis()
        if (action == MotionEvent.ACTION_DOWN) touchStart = now
        val points = event.getJSONArray("points")
        val properties = Array(points.length()) { i -> MotionEvent.PointerProperties().apply {
            id = points.getJSONObject(i).getInt("id")
            toolType = MotionEvent.TOOL_TYPE_FINGER
        } }
        val coords = Array(points.length()) { i -> MotionEvent.PointerCoords().apply {
            x = points.getJSONObject(i).getDouble("x").toFloat()
            y = points.getJSONObject(i).getDouble("y").toFloat()
            pressure = 1f; size = 0.1f
        } }
        val motion = MotionEvent.obtain(touchStart, now, action, points.length(), properties, coords,
            0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
        try { check(runner.uiAutomation.injectInputEvent(motion, true)) } finally { motion.recycle() }
    }
}
