package com.rm.frond

import android.content.Context
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Bundle
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.print.PageRange
import android.print.PrintAttributes
import android.print.PrintDocumentAdapter
import android.print.PrintDocumentInfo
import android.print.pdf.PrintedPdfDocument
import java.io.FileOutputStream

// Draws PrintJob pages for PrintManager, one text page per paper page in a monospaced font sized so 80
// columns by 66 lines fit the page's printable area.
class PagesAdapter(private val context: Context, private val name: String, private val pages: List<List<String>>) : PrintDocumentAdapter() {
    private var attributes: PrintAttributes? = null
    private val count get() = pages.size.coerceAtLeast(1)

    override fun onLayout(old: PrintAttributes?, new: PrintAttributes, cancel: CancellationSignal, callback: LayoutResultCallback, extras: Bundle?) {
        if (cancel.isCanceled) { callback.onLayoutCancelled(); return }
        attributes = new
        callback.onLayoutFinished(
            PrintDocumentInfo.Builder(name).setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT).setPageCount(count).build(),
            old != new,
        )
    }

    override fun onWrite(ranges: Array<out PageRange>, destination: ParcelFileDescriptor, cancel: CancellationSignal, callback: WriteResultCallback) {
        val attrs = attributes
        if (attrs == null) { callback.onWriteFailed("No page size."); return }
        val document = PrintedPdfDocument(context, attrs)
        try {
            val paint = Paint().apply { typeface = Typeface.MONOSPACE; textSize = 100f; isAntiAlias = true }
            val advance = paint.measureText("0") / 100f
            val spacing = paint.fontSpacing / 100f
            val ascent = -paint.ascent() / 100f
            var written = 0
            for (index in 0 until count) {
                if (cancel.isCanceled) { callback.onWriteCancelled(); return }
                if (ranges.none { index >= it.start && index <= it.end }) continue
                val page = document.startPage(written++)
                val area = page.info.contentRect
                val size = PrintJob.fontSize(area.width().toFloat(), area.height().toFloat(), advance, spacing)
                paint.textSize = size
                var y = area.top + ascent * size
                for (line in pages.getOrElse(index) { emptyList() }) {
                    page.canvas.drawText(line, area.left.toFloat(), y, paint)
                    y += spacing * size
                }
                document.finishPage(page)
            }
            FileOutputStream(destination.fileDescriptor).use { document.writeTo(it) }
            callback.onWriteFinished(ranges.toList().toTypedArray())
        } catch (e: Exception) {
            callback.onWriteFailed(e.message)
        } finally {
            document.close()
        }
    }
}
