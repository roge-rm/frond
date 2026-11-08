package com.rm.frond

import java.io.File
import java.io.RandomAccessFile

// The sectors of a fresh card as the card tool's `make` writes them (the core's frondSd::layout),
// for tests that need a new card without the tool: a raw image or a v2 card file.
object CoreLayout {
    private fun put16(b: ByteArray, at: Int, v: Int) { b[at] = v.toByte(); b[at + 1] = (v shr 8).toByte() }
    private fun put32(b: ByteArray, at: Int, v: Long) { put16(b, at, (v and 0xffff).toInt()); put16(b, at + 2, (v shr 16).toInt()) }

    // The non-zero sectors of a fresh card by sector number, as the core's frondSd::layout makes them.
    fun layout(mib: Int): Map<Long, ByteArray> {
        val start = 63L
        val total = (mib.toLong() shl 20) / 512
        val sectors = total - start
        val cluster = if (mib <= 64) 4 else if (mib <= 256) 8 else if (mib == 512) 16 else if (mib == 1024) 32 else 64
        val aligned = mib > 128
        var fatSectors = 1
        var reserved = 1
        var clusters: Long
        while (true) {
            clusters = minOf((sectors - reserved - 32 - 2 * fatSectors) / cluster, 65524L)
            val required = ((clusters + 2) * 2 + 511).toInt() / 512
            if (fatSectors >= required) break
            fatSectors = required
        }
        if (aligned) reserved = (total - clusters * cluster - start - 32 - 2 * fatSectors).toInt()
        val out = LinkedHashMap<Long, ByteArray>()
        val mbr = ByteArray(512)
        mbr[446 + 1] = 1; mbr[446 + 2] = 1
        mbr[446 + 4] = 6; mbr[446 + 5] = 254.toByte(); mbr[446 + 6] = 255.toByte(); mbr[446 + 7] = 255.toByte()
        put32(mbr, 446 + 8, start); put32(mbr, 446 + 12, sectors)
        mbr[510] = 0x55; mbr[511] = 0xaa.toByte()
        out[0] = mbr
        val boot = ByteArray(512)
        boot[0] = 0xeb.toByte(); boot[1] = 0x3c; boot[2] = 0x90.toByte()
        "FROND001".toByteArray(Charsets.US_ASCII).copyInto(boot, 3)
        put16(boot, 11, 512); boot[13] = cluster.toByte(); put16(boot, 14, reserved)
        boot[16] = 2; put16(boot, 17, 512)
        if (sectors < 65536) put16(boot, 19, sectors.toInt()) else put32(boot, 32, sectors)
        boot[21] = 0xf8.toByte(); put16(boot, 22, fatSectors); put16(boot, 24, 63); put16(boot, 26, 255)
        put32(boot, 28, start); boot[36] = 0x80.toByte(); boot[38] = 0x29; put32(boot, 39, 0x46524f4e)
        "FROND      ".toByteArray(Charsets.US_ASCII).copyInto(boot, 43)
        "FAT16   ".toByteArray(Charsets.US_ASCII).copyInto(boot, 54)
        boot[510] = 0x55; boot[511] = 0xaa.toByte()
        out[start] = boot
        for (fat in 0 until 2) {
            val sector = ByteArray(512)
            put16(sector, 0, 0xfff8); put16(sector, 2, 0xffff)
            out[start + reserved + fat.toLong() * fatSectors] = sector
        }
        val root = ByteArray(512)
        "FROND      ".toByteArray(Charsets.US_ASCII).copyInto(root, 0)
        root[11] = 8
        out[start + reserved + 2L * fatSectors] = root
        return out
    }

    // A raw image of a fresh `mib` MiB card, as a sparse file.
    fun blank(file: File, mib: Int) {
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(mib.toLong() shl 20)
            for ((sector, bytes) in layout(mib)) {
                raf.seek(sector * 512)
                raf.write(bytes)
            }
        }
    }

    // A v2 card file of a fresh `mib` MiB card, as `make` leaves it.
    fun card(file: File, mib: Int) {
        val payload = ByteArray(mib shl 20)
        for ((sector, bytes) in layout(mib)) bytes.copyInto(payload, (sector * 512).toInt())
        file.writeBytes(CardFile.encodeV2(payload, 1))
    }
}
