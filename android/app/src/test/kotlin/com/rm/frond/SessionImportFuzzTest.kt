package com.rm.frond

import com.code_intelligence.jazzer.junit.FuzzTest
import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.File

// What a picked session brings in: a zip of packable files, the .frond's card descriptors against
// the card files beside it, and the machine settings read from session JSON.
class SessionImportFuzzTest {
    private fun <T> inFolder(work: (File) -> T): T {
        val root = fuzzTempDir("frond-import-fuzz")
        try {
            return work(root)
        } finally {
            root.deleteRecursively()
        }
    }

    @FuzzTest
    fun settings(data: ByteArray) {
        val json = try { JSONObject(String(data, Charsets.UTF_8)) } catch (_: Exception) { return }
        val s = MachineSettings.from(json)
        check(s.contrast in 0..100 && s.batteryLevel in 0..100)
        val again = MachineSettings.from(s.json())
        check(again == s) { "settings changed on a save: $s then $again" }
    }

    @FuzzTest
    fun unzip(data: ByteArray) = inFolder { dir ->
        unzipPackable(ByteArrayInputStream(data), dir, nandMax = 1L shl 16)
        for (f in dir.listFiles().orEmpty()) {
            check(f.isFile && (packableName(f.name) || f.name == "sd0.frondsd" || f.name == NAND_FILE)) { "unpacked ${f.name}" }
            if (f.name == NAND_FILE) check(f.length() <= 1L shl 16)
        }
    }

    // The first two bytes split the input into the .frond, an SD card file and a stick file; the
    // third sets swap.
    @FuzzTest
    fun siblings(data: ByteArray) = inFolder { dir ->
        if (data.size < 3) return@inFolder
        val a = 3 + (data[0].toInt() and 0xff) * 4
        val b = a + (data[1].toInt() and 0xff) * 4
        val frond = File(dir, "session.frond")
        frond.writeBytes(data.copyOfRange(3, minOf(a, data.size)))
        if (data.size > a) File(dir, "sd.frondsd").writeBytes(data.copyOfRange(a, minOf(b, data.size)))
        if (data.size > b) File(dir, "ms.frondms").writeBytes(data.copyOfRange(b, data.size))
        checkImportedSiblings(dir, frond, swap = data[2].toInt() and 1 != 0)
    }
}
