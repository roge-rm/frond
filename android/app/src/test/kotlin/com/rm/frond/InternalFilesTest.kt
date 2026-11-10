package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class InternalFilesTest {
    private fun info(name: String, resource: Boolean) = PalmDatabases.Info(name, if (resource) "appl" else "DATA", "test", resource)

    // Databases list by file name: .prc or .pdb, unsafe characters made _, a repeat gets " (2)".
    @Test fun databasesListByFileName() {
        assertEquals("Memo_DB.pdb", InternalFiles.fileName(info("Memo/DB", false)))
        assertEquals("a_b_c_.prc", InternalFiles.fileName(info("a:b*c?", true)))
        assertEquals("Database.pdb", InternalFiles.fileName(info("..", false)))
        val all = listOf(info("A/B", false), info("A:B", false), info("Notes", true))
        val (entries, by) = InternalFiles.entriesOf(all)
        assertEquals(listOf("A_B.pdb", "A_B (2).pdb", "Notes.prc"), entries.map { it.name })
        assertEquals("A:B", by["A_B (2).pdb"]?.name)
        assertEquals(entries.map { it.name }, entries.map { it.shortName })
    }

    // An export's ZIP comes out as its files, by their own names, folders in the ZIP left out.
    @Test fun anExportUnzipsByName() {
        val zip = ByteArrayOutputStream()
        ZipOutputStream(zip).use { z ->
            for ((name, data) in listOf("Memos.pdb" to "memo", "sub/" to "", "sub/Notes.prc" to "note")) {
                z.putNextEntry(ZipEntry(name))
                z.write(data.toByteArray())
                z.closeEntry()
            }
        }
        val out = LinkedHashMap<String, ByteArrayOutputStream>()
        val n = InternalFiles.unzip(ByteArrayInputStream(zip.toByteArray())) { name -> ByteArrayOutputStream().also { out[name] = it } }
        assertEquals(2, n)
        assertEquals(listOf("Memos.pdb", "Notes.prc"), out.keys.toList())
        assertEquals("note", out["Notes.prc"].toString())
    }
}
