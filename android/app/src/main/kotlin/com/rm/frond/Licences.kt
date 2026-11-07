package com.rm.frond

import android.content.Context

/** One entry of the licence list: the asset files holding its notice, a name and its licence. */
data class LicenceEntry(val files: List<String>, val name: String, val licence: String)

/** The third-party notices in the app's licences/ assets (the repo's licences/ folder, plus GPL-3.0.txt). */
object Licences {
    const val GPL = "GPL-3.0.txt"
    private const val INDEX = "index.tsv"

    /** Lines of index.tsv for the Android app: files (comma separated), name, licence, apps. */
    fun parse(index: String): List<LicenceEntry> = index.lines().filter { it.isNotBlank() }.mapNotNull { line ->
        val f = line.split('\t')
        if (f.size < 4 || 'a' !in f[3]) null else LicenceEntry(f[0].split(','), f[1], f[2])
    }

    fun text(context: Context, file: String): String =
        context.assets.open("licences/$file").bufferedReader().use { it.readText() }

    fun entries(context: Context): List<LicenceEntry> = parse(text(context, INDEX))

    fun text(context: Context, entry: LicenceEntry): String =
        entry.files.joinToString("\n\n") { text(context, it).trim() }
}
