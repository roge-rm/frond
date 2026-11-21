package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class SyncFilesTest {
    @Test fun desktopCsvKeepsOpaqueValuesAndUnicode() {
        val input = "kind,category,private,archived,fields,extras\r\n" +
            "\"memos\",\"Words, clauses\nQuotes\"\",\",\"1\",\"1\",\"{\"\"text\"\":\"\"Té 🫖\"\"}\",\"{\"\"Bd01\"\":\"\"AB3/TTw=\"\"}\"\r\n"
        val memo = SyncFiles.read("desktop.csv", SyncKind.MEMOS, input).getJSONObject(0)
        assertEquals("Words, clauses\nQuotes\",", memo.getString("category"))
        assertEquals("Té 🫖", memo.getJSONObject("fields").getString("text"))
        assertEquals("001dff4d3c", memo.getJSONObject("extras").getString("Bd01"))
        assertTrue(memo.getBoolean("private"))
        assertTrue(memo.getBoolean("archived"))
        val exported = SyncFiles.write("csv", SyncKind.MEMOS, listOf(memo))
        assertEquals("001dff4d3c", SyncFiles.read("mobile.csv", SyncKind.MEMOS, exported).getJSONObject(0)
            .getJSONObject("extras").getString("Bd01"))
        assertThrows(IllegalArgumentException::class.java) { SyncFiles.read("mobile.csv", SyncKind.TASKS, exported) }
    }

    @Test fun vCardKeepsFoldedUtf8AndCommonFields() {
        val note = "Édith, 🫖\n".repeat(12)
        val card = JSONObject().put("fields", JSONObject().put("firstName", "Édith")
            .put("lastName", "A;B").put("note", note).put("phone1", "a@example.test").put("phoneLabel1", "4"))
            .put("category", "Work, Home").put("private", true)
        val data = SyncFiles.write("vcf", SyncKind.CONTACTS, listOf(card))
        assertTrue("\r\n " in data)
        assertTrue(data.lineSequence().all { it.toByteArray().size <= 75 })
        val result = SyncFiles.read("mobile.vcf", SyncKind.CONTACTS, data).getJSONObject(0)
        assertEquals("Work, Home", result.getString("category"))
        assertTrue(result.getBoolean("private"))
        val fields = result.getJSONObject("fields")
        assertEquals(note, fields.getString("note"))
        assertEquals("A;B", fields.getString("lastName"))
        assertEquals("4", fields.getString("phoneLabel1"))
    }

    @Test fun calendarKeepsTimesAndRejectsRecurrences() {
        val event = JSONObject().put("fields", JSONObject().put("summary", "An event")
            .put("date", "2026-11-20").put("startTime", "09:01").put("endTime", "10:02"))
        val data = SyncFiles.write("ics", SyncKind.CALENDAR, listOf(event))
        val back = SyncFiles.read("mobile.ics", SyncKind.CALENDAR, data).getJSONObject(0)
        assertEquals("09:01", back.getJSONObject("fields").getString("startTime"))
        assertEquals("10:02", back.getJSONObject("fields").getString("endTime"))
        assertFalse(back.getBoolean("private"))
        val repeating = data.replace("END:VEVENT", "RRULE:FREQ=DAILY\r\nEND:VEVENT")
        assertThrows(IllegalArgumentException::class.java) { SyncFiles.read("repeat.ics", SyncKind.CALENDAR, repeating) }
    }

    @Test fun memoTextIsKeptExactly() {
        val data = "Heading\n\nLast line\r\n"
        val memo = SyncFiles.read("memo.txt", SyncKind.MEMOS, data).getJSONObject(0)
        assertEquals(data, SyncFiles.write("txt", SyncKind.MEMOS, listOf(memo)))
    }
}
