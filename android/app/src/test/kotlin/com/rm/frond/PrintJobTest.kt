package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PrintJobTest {
    private fun r(s: String) = PrintJob.render(s.toByteArray(Charsets.ISO_8859_1))
    private fun pages(s: String) = r(s).pages

    @Test fun statusParses() {
        val s = PrintJob.parseStatus("attached collecting bytes=289 last=none idle=3500")
        assertTrue(s.valid && s.attached)
        assertEquals("collecting", s.state)
        assertEquals(289L, s.bytes)
        assertEquals("none", s.last)
        assertEquals(3500L, s.idleMs)
        assertEquals("", s.error)
        val off = PrintJob.parseStatus("printer: nothing is attached off idle bytes=0 last=none idle=-1")
        assertTrue(off.valid && !off.attached)
        assertEquals(-1L, off.idleMs)
        assertEquals("printer: nothing is attached", off.error)
        assertFalse(PrintJob.parseStatus("garbage").valid)
    }

    @Test fun receivedAfterThePause() {
        fun st(attached: String, bytes: Int, idle: Int) = PrintJob.parseStatus("$attached collecting bytes=$bytes last=none idle=$idle")
        assertFalse(st("attached", 0, -1).received)
        assertFalse(st("attached", 10, 2999).received)
        assertTrue(st("attached", 10, 3000).received)
        assertFalse(st("off", 10, 5000).received)
        assertFalse(PrintJob.parseStatus("nonsense").received)
    }

    @Test fun rowFollowsTheCapability() {
        for (device in Device.entries) assertEquals(device.id, device.usbPrinter, PrintJob.rowShown(device))
        assertTrue(Device.entries.any { it.usbPrinter })
    }

    @Test fun lineEnds() {
        assertEquals(listOf(listOf("a", "b")), pages("a\r\nb\r\n"))
        assertEquals(listOf(listOf("a", "b")), pages("a\nb"))
        assertEquals(listOf(listOf("a", "b")), pages("a\rb"))
        assertEquals(listOf(listOf("a", "", "b")), pages("a\r\rb"))
        assertEquals(listOf(listOf("a", "")), pages("a\n\r"))
        assertTrue(r("").renderable && r("").pages.isEmpty())
    }

    @Test fun formFeeds() {
        assertEquals(listOf(listOf("a"), listOf("b")), pages("a\r\n\u000cb\r\n"))
        assertEquals(listOf(listOf("a"), listOf("b")), pages("a\u000cb"))
        assertEquals(listOf(listOf("a")), pages("a\r\n\u000c"))
        assertEquals(listOf(listOf("a"), emptyList<String>()), pages("a\u000c\u000c"))
    }

    @Test fun tabsAndWrap() {
        assertEquals("a       b", pages("a\tb")[0][0])
        assertEquals("12345678        b", pages("12345678\tb")[0][0])
        assertEquals(listOf("x".repeat(80)), pages("x".repeat(80) + "\r\n")[0])
        assertEquals(listOf("x".repeat(80), "x"), pages("x".repeat(81) + "\r\n")[0])
        assertEquals(2, pages("x".repeat(160))[0].size)
        assertEquals(listOf("x".repeat(80), "        |"), pages("x".repeat(80) + "\t|")[0])
    }

    @Test fun pageLength() {
        val s66 = "l\r\n".repeat(66)
        assertEquals(1, pages(s66).size)
        assertEquals(66, pages(s66)[0].size)
        assertEquals(1, pages(s66 + "\u000c").size)
        val s67 = pages(s66 + "last\r\n")
        assertEquals(2, s67.size)
        assertEquals(listOf("last"), s67[1])
        assertEquals(3, pages("l\r\n".repeat(140)).size)
    }

    @Test fun controlBytesAreNotText() {
        for (c in listOf(0x00, 0x01, 0x07, 0x08, 0x0b, 0x1b, 0x1f, 0x7f)) {
            val t = r("ab" + c.toChar() + "cd")
            assertTrue(!t.renderable && t.pages.isEmpty())
        }
        assertEquals("Only the Generic printer profile can be shown as pages", r("\u001bE").reason)
    }

    @Test fun highBytes() {
        assertEquals("é", pages("é")[0][0])
        assertEquals("€", pages("\u0080")[0][0])
        assertEquals("’", pages("\u0092")[0][0])
        assertEquals("�", pages("\u0081")[0][0])
        assertEquals("a b", pages("a b")[0][0])
        assertEquals(1, pages("é".repeat(80))[0].size)
    }

    @Test fun testPageShape() {
        val t = r("Palm OS Test Page\r\n\r\nGeneric printer\r\n  tab\there\r\n\u000c")
        assertTrue(t.renderable)
        assertEquals(listOf(listOf("Palm OS Test Page", "", "Generic printer", "  tab   here")), t.pages)
    }

    @Test fun fontFitsBothLimits() {
        // 66 lines at 1.2 spacing are taller than 80 columns at 0.6 are wide, so a portrait page is limited by its height.
        assertEquals(842f / 79.2f, PrintJob.fontSize(595f, 842f), 0.001f)
        assertEquals(500f / 48f, PrintJob.fontSize(500f, 2000f), 0.001f)
    }
}
