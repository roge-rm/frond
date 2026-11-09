package com.rm.frond

// The Dana's USB printer in the app: the core's status line, when a job counts as received, and the
// Generic (6.0) text renderer (the same rules as print/PrintText.h).
object PrintJob {
    const val COLUMNS = 80
    const val LINES_PER_PAGE = 66
    const val NOT_TEXT = "Only the Generic printer profile can be shown as pages"

    // A job counts as received when bytes have come in and then stopped for this long.
    const val PAUSE_MS = 3000L

    // "[error text ]attached|off STATE bytes=N last=LAST idle=MS"; valid is false for anything else.
    data class Status(
        val valid: Boolean = false, val attached: Boolean = false, val state: String = "", val last: String = "",
        val error: String = "", val bytes: Long = 0, val idleMs: Long = -1,
    ) {
        val received get() = valid && attached && bytes > 0 && idleMs >= PAUSE_MS
    }

    private val pattern = Regex("""(attached|off) (\S+) bytes=(\d+) last=(\S+) idle=(-?\d+)\s*$""")

    fun parseStatus(message: String): Status {
        val m = pattern.find(message) ?: return Status()
        val (attached, state, bytes, last, idle) = m.destructured
        return Status(true, attached == "attached", state, last, message.substring(0, m.range.first).trim(),
            bytes.toLongOrNull() ?: 0, idle.toLongOrNull() ?: -1)
    }

    // The Dana has a printer to plug in.
    fun rowShown(device: Device) = device.usbPrinter

    class Pages(val renderable: Boolean, val reason: String, val pages: List<List<String>>)

    private val high = intArrayOf(
        0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
        0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178)

    // CR, LF and CRLF end a line, form feed a page, tab goes to the next multiple of 8, lines wrap at 80
    // columns and pages hold 66 lines. A line ending exactly at a limit adds nothing until more text comes.
    // Any other control byte means the job isn't plain text. 0x80-0xff read as Windows-1252, 0xa0 as a space.
    fun render(data: ByteArray, size: Int = data.size): Pages {
        val pages = ArrayList<List<String>>()
        var page = ArrayList<String>()
        val line = StringBuilder()
        var col = 0
        var lineOpen = false
        var pageUsed = false
        fun endLine() {
            if (page.size >= LINES_PER_PAGE) { pages.add(page); page = ArrayList() }
            page.add(line.toString())
            line.setLength(0)
            col = 0
            lineOpen = false
        }
        var i = 0
        while (i < size) {
            val b = data[i].toInt() and 0xff
            when {
                b == 13 || b == 10 -> {
                    if (b == 13 && i + 1 < size && data[i + 1].toInt() == 10) i++
                    endLine()
                    pageUsed = true
                }
                b == 12 -> {
                    if (lineOpen) endLine()
                    pages.add(page)
                    page = ArrayList()
                    pageUsed = false
                }
                b == 9 -> {
                    if (col >= COLUMNS) endLine()
                    val next = (col / 8 + 1) * 8
                    repeat(next - col) { line.append(' ') }
                    col = next
                    lineOpen = true
                    pageUsed = true
                }
                b < 0x20 || b == 0x7f -> return Pages(false, NOT_TEXT, emptyList())
                else -> {
                    if (col >= COLUMNS) endLine()
                    when {
                        b < 0x7f -> line.append(b.toChar())
                        b < 0xa0 -> line.appendCodePoint(high[b - 0x80])
                        b == 0xa0 -> line.append(' ')
                        else -> line.append(b.toChar())
                    }
                    col++
                    lineOpen = true
                    pageUsed = true
                }
            }
            i++
        }
        if (lineOpen) endLine()
        if (pageUsed && page.isNotEmpty()) pages.add(page)
        return Pages(true, "", pages)
    }

    // The monospaced text size that fits 80 columns by 66 lines on a page of the given size in points,
    // for a font whose advance is `advance` and line spacing `spacing` times its size.
    fun fontSize(widthPt: Float, heightPt: Float, advance: Float = 0.6f, spacing: Float = 1.2f): Float =
        minOf(widthPt / (COLUMNS * advance), heightPt / (LINES_PER_PAGE * spacing))
}
