package com.rm.frond

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream

class BoundedCardImageTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun exactLimitCopiesWithoutChangingTheBytes() {
        val bytes = ByteArray(131_073) { (it * 37).toByte() }
        val target = temporary.newFile()
        assertTrue(BoundedCardImage.copy(ByteArrayInputStream(bytes), target, bytes.size.toLong()))
        assertArrayEquals(bytes, target.readBytes())
    }

    @Test fun unknownSizeStreamStopsAfterTheFirstExcessByteAndLeavesNoFile() {
        var read = 0
        val endless = object : InputStream() {
            override fun read(): Int { read++; return 42 }
            override fun read(bytes: ByteArray, offset: Int, count: Int): Int {
                bytes.fill(42, offset, offset + count); read += count; return count
            }
        }
        val target = temporary.newFile()
        assertFalse(BoundedCardImage.copy(endless, target, 7))
        assertEquals(8, read)
        assertFalse(target.exists())
    }

    @Test fun providerFailureRemovesThePartialCopy() {
        var calls = 0
        val failed = object : InputStream() {
            override fun read(): Int = throw IOException("provider closed")
            override fun read(bytes: ByteArray, offset: Int, count: Int): Int {
                if (++calls > 1) throw IOException("provider closed")
                bytes[offset] = 1; return 1
            }
        }
        val target = temporary.newFile()
        try {
            BoundedCardImage.copy(failed, target, 20)
            fail("Expected provider failure")
        } catch (_: IOException) { }
        assertFalse(target.exists())
    }

    @Test fun emptyReadDoesNotSpinOrLoseTheFollowingByte() {
        val input = object : ByteArrayInputStream(byteArrayOf(5, 8)) {
            private var empty = true
            override fun read(bytes: ByteArray, offset: Int, count: Int): Int {
                if (empty) { empty = false; return 0 }
                return super.read(bytes, offset, count)
            }
        }
        val target = temporary.newFile()
        assertTrue(BoundedCardImage.copy(input, target, 2))
        assertArrayEquals(byteArrayOf(5, 8), target.readBytes())
    }
}
