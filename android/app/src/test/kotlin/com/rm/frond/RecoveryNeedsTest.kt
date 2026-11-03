package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RecoveryNeedsTest {
    @Test fun savedSessionNeedsItsSizeWithSlack() {
        val size = 43L shl 20
        val needs = Sessions.recoveryNeeds(size, Device.TX)
        assertTrue(needs > size)
        assertEquals(size + size / 4 + (16L shl 20), needs)
    }

    @Test fun unsavedSessionNeedsItsFirmware() {
        assertEquals(Device.E2.norSize + Device.E2.nandSize + (64L shl 20), Sessions.recoveryNeeds(null, Device.E2))
        assertEquals(Sessions.recoveryNeeds(null, Device.TX), Sessions.recoveryNeeds(0, Device.TX))
    }

    // Exchange room comes from the real card: a live candidate or sd.frondsd, at least 128 MB.
    @Test fun exchangeNeedsTheRealCardSize() {
        val dir = kotlin.io.path.createTempDirectory("exchange").toFile()
        try {
            val small = FatImage.SMALL_MAX_BYTES
            assertEquals(Sessions.recoveryNeeds(null, Device.TX) + 2 * small + SdCard.JOURNAL_ROOM, SdCard.exchangeNeeds(null, Device.TX, dir))
            val saved = 300L shl 20
            assertEquals(Sessions.recoveryNeeds(saved, Device.TX) + small + SdCard.JOURNAL_ROOM, SdCard.exchangeNeeds(saved, Device.TX, dir))
            java.io.RandomAccessFile(java.io.File(dir, "sd.frondsd"), "rw").use { it.setLength((256L shl 20) + 64) }
            assertEquals(Sessions.recoveryNeeds(saved, Device.TX) + (256L shl 20) + 64 + SdCard.JOURNAL_ROOM, SdCard.exchangeNeeds(saved, Device.TX, dir))
            // A 2 GiB card made since the last save isn't in the saved session yet.
            val live = (2048L shl 20) + 64 + 8192
            java.io.RandomAccessFile(java.io.File(dir, ".frondsd.tmp-live-x1y2z3"), "rw").use { it.setLength(live) }
            assertEquals(SdCard.cardBytes(dir), live to true)
            assertEquals(Sessions.recoveryNeeds(saved, Device.TX) + 2 * live + SdCard.JOURNAL_ROOM, SdCard.exchangeNeeds(saved, Device.TX, dir))
            assertEquals(Sessions.recoveryNeeds(null, Device.TX) + 2 * small + SdCard.JOURNAL_ROOM, SdCard.exchangeNeeds(null, Device.TX, null))
        } finally {
            dir.deleteRecursively()
        }
    }
}
