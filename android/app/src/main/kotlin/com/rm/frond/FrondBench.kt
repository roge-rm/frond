package com.rm.frond

import android.content.Context
import android.net.Uri
import java.io.InputStream

/** FrondBench, the benchmark app, bundled as an asset from the committed tools/frondbench/FrondBench.prc. */
object FrondBench {
    const val ASSET = "frondbench/FrondBench.prc"
    const val FILE_NAME = "FrondBench.prc"

    fun open(context: Context): InputStream = context.assets.open(ASSET)

    /** Writes the PRC to a file the user picked. Returns false when it couldn't. */
    fun save(context: Context, to: Uri): Boolean = try {
        val out = context.contentResolver.openOutputStream(to) ?: return false
        out.use { o -> open(context).use { it.copyTo(o) } }
        true
    } catch (_: java.io.IOException) {
        false
    }
}
