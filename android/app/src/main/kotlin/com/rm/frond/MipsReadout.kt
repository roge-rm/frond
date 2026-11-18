package com.rm.frond

// Where the speed readout sits: the bottom right corner of the Palm as the core draws it.
object MipsReadout {
    // The core's window-layout reply: the device's rectangle in the window, and the window's size.
    data class Layout(val x: Int, val y: Int, val width: Int, val height: Int, val windowWidth: Int, val windowHeight: Int)

    fun parseLayout(text: String): Layout? {
        val n = text.trim().split(' ').map { it.toIntOrNull() ?: return null }
        if (n.size != 6 || n[2] <= 0 || n[3] <= 0 || n[4] <= 0 || n[5] <= 0) return null
        return Layout(n[0], n[1], n[2], n[3], n[4], n[5])
    }

    // The figure in a speed-state reply ("312.00 MHz, limit ..."; the Zodiac's is in MIPS), rounded; null when it isn't there.
    fun parseMips(text: String): Long? {
        val head = text.substringBefore(',').trim()
        if (!head.endsWith(" MHz") && !head.endsWith(" MIPS")) return null
        return head.substringBefore(' ').toDoubleOrNull()?.let { Math.round(it) }
    }

    // The unit of that figure: "MHz" for a core clock, "MIPS" where the board reports instructions a second.
    fun unit(text: String): String = if (text.substringBefore(',').trim().endsWith(" MIPS")) "MIPS" else "MHz"

    // Right and bottom margins in the parent, for a view whose surface sits at (left, top) with the given size
    // and shows the core's window scaled to fit it. inset keeps the box off the edge.
    fun margins(layout: Layout, left: Int, top: Int, width: Int, height: Int, parentWidth: Int, parentHeight: Int, inset: Int): Pair<Int, Int> {
        val right = left + (layout.x + layout.width).toLong() * width / layout.windowWidth
        val bottom = top + (layout.y + layout.height).toLong() * height / layout.windowHeight
        return Pair(maxOf(0, parentWidth - right.toInt() + inset), maxOf(0, parentHeight - bottom.toInt() + inset))
    }
}
