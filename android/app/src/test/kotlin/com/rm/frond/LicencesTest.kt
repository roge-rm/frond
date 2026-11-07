package com.rm.frond

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LicencesTest {
    // Unit tests run in android/app, the licences are two folders up.
    private val folder = File("../../licences")

    @Test fun everyListedFileExists() {
        val lines = File(folder, "index.tsv").readText()
        val entries = Licences.parse(lines)
        assertTrue(entries.size > 10)
        for (line in lines.lines().filter { it.isNotBlank() }) {
            for (file in line.split('\t')[0].split(',')) assertTrue(file, File(folder, file).isFile)
        }
        assertTrue(File("../../LICENSE").isFile)
    }

    @Test fun androidListSkipsDesktopOnly() {
        val entries = Licences.parse("a-file\tA\tMIT\tda\nb-file\tB\tMIT\td\nc-file,d-file\tC\tMIT\ta\n")
        assertEquals(listOf("A", "C"), entries.map { it.name })
        assertEquals(listOf("c-file", "d-file"), entries[1].files)
    }
}
