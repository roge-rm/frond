package com.rm.frond

import android.content.Context
import java.io.File

// The core's output for the running session. android_main.cpp writes each line to files/core.log,
// started fresh on each run and moved to core.log.1 past 256 KB.
object CoreLog {
    // Enough to show and to copy or share in one go.
    const val MAX_CHARS = 100_000

    fun read(context: Context): String {
        val files = listOf(File(context.filesDir, "core.log.1"), File(context.filesDir, "core.log"))
        val parts = files.map { f -> try { if (f.isFile) f.readText() else "" } catch (e: Exception) { "" } }
        return tail(parts.joinToString(""), MAX_CHARS)
    }

    // The last max characters of text, starting at a whole line when it has to cut.
    fun tail(text: String, max: Int): String {
        if (text.length <= max) return text
        val cut = text.length - max
        val line = text.indexOf('\n', cut)
        return if (line < 0 || line + 1 >= text.length) text.substring(cut) else text.substring(line + 1)
    }
}
