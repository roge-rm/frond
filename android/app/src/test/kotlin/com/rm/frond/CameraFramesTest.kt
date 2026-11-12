package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class CameraFramesTest {
    // A buffer laid out as the core makes the frames file.
    private fun coreFile(): ByteBuffer {
        val b = ByteBuffer.allocate(CameraFrames.HEADER_BYTES + CameraFrames.CAPACITY).order(ByteOrder.LITTLE_ENDIAN)
        b.put("FrondCam".toByteArray()).putInt(8, 1).putInt(12, CameraFrames.CAPACITY)
        return b
    }

    @Test fun rowSaysTheChoiceOrWhyItStopped() {
        assertEquals("Test pattern", CameraFrames.rowText(CameraFrames.Choice.PATTERN, "pattern"))
        assertEquals("Picture", CameraFrames.rowText(CameraFrames.Choice.PICTURE, "frames"))
        assertEquals("Phone camera", CameraFrames.rowText(CameraFrames.Choice.PHONE, "frames"))
        assertEquals("Permission denied", CameraFrames.rowText(CameraFrames.Choice.PATTERN, "pattern Permission denied"))
        assertEquals(listOf("Test pattern", "Picture…", "Phone camera"), CameraFrames.choices.map { CameraFrames.choiceLabel(it) })
    }

    // A queue run by hand, standing in for the sending thread.
    private class Queue : java.util.concurrent.Executor {
        val tasks = ArrayDeque<Runnable>()
        override fun execute(command: Runnable) { tasks.add(command) }
        fun runAll() { while (tasks.isNotEmpty()) tasks.removeFirst().run() }
    }

    @Test fun aReplacedChoiceNeverReachesTheCore() {
        val queue = Queue()
        val sent = mutableListOf<String>()
        val modes = CameraFrames.Modes(queue) { mode, _ -> sent.add(mode); ControlLink.Reply(true, "camera", mode) }
        val replies = mutableListOf<ControlLink.Reply?>()
        // A picture choice that's still decoding when the app is left: its frames come after the pause's pattern.
        val picture = modes.next()
        val paused = modes.next()
        modes.send(paused, "pattern", 2) { replies.add(it) }
        modes.send(picture, "frames", 10) { replies.add(it) }
        queue.runAll()
        assertEquals(listOf("pattern"), sent)
        assertEquals("pattern", replies[0]?.message)
        assertNull(replies[1])
        // Queued in time but replaced before it ran: dropped too.
        val phone = modes.next()
        modes.send(phone, "frames", 10) { replies.add(it) }
        modes.send(modes.next(), "pattern", 2) { replies.add(it) }
        queue.runAll()
        assertEquals(listOf("pattern", "pattern"), sent)
        assertNull(replies[2])
        // Sent before the pause: the pause's pattern still goes after it.
        val again = modes.next()
        modes.send(again, "frames", 10)
        queue.runAll()
        modes.send(modes.next(), "pattern", 2)
        queue.runAll()
        assertEquals(listOf("pattern", "pattern", "frames", "pattern"), sent)
    }

    @Test fun onlyFramesKeepTheFeed() {
        assertTrue(CameraFrames.stillFeeding("frames"))
        for (state in listOf("pattern", "picture", "webcam", "pattern Wake the device to use the camera")) assertFalse(state, CameraFrames.stillFeeding(state))
    }

    @Test fun writeFollowsTheSequenceProtocol() {
        val b = coreFile()
        assertTrue(CameraFrames.valid(b))
        var sequenceWhileWriting = -1
        assertTrue(CameraFrames.write(b, CameraFrames.FORMAT_RGBA, 2, 1, 90, CameraFrames.FLAG_MIRROR) { out ->
            sequenceWhileWriting = b.getInt(16)
            CameraFrames.putRgba(intArrayOf(0x80112233.toInt(), 0x00445566), out)
        })
        assertEquals(1, sequenceWhileWriting)
        assertEquals(2, b.getInt(16))
        assertEquals(listOf(CameraFrames.FORMAT_RGBA, 2, 1, 90, 1), (20..36 step 4).map { b.getInt(it) })
        val pixels = ByteArray(8).also { b.position(CameraFrames.HEADER_BYTES); b.get(it) }
        assertArrayEquals(byteArrayOf(0x11, 0x22, 0x33, 0x80.toByte(), 0x44, 0x55, 0x66, 0), pixels)
        assertTrue(CameraFrames.write(b, CameraFrames.FORMAT_I420, 2, 2, 0, 0) { it.put(ByteArray(6)) })
        assertEquals(4, b.getInt(16))
        // Too big, or a bad size: nothing changes.
        assertFalse(CameraFrames.write(b, CameraFrames.FORMAT_RGBA, 1281, 960, 0, 0) {})
        assertFalse(CameraFrames.write(b, 9, 2, 2, 0, 0) {})
        assertEquals(4, b.getInt(16))
    }

    @Test fun anOldOrForeignFileIsRefused() {
        val b = coreFile()
        b.putInt(12, 100)
        assertFalse(CameraFrames.valid(b))
        assertFalse(CameraFrames.valid(ByteBuffer.allocate(64)))
    }

    @Test fun planesArePackedTight() {
        // A 4 x 2 plane with 8-byte rows, then a chroma plane of 2 x 1 with pixel stride 2 (interleaved).
        val y = ByteBuffer.wrap(byteArrayOf(1, 2, 3, 4, 9, 9, 9, 9, 5, 6, 7, 8, 9, 9, 9, 9))
        val out = ByteBuffer.allocate(8)
        CameraFrames.putPlane(y, 8, 1, 4, 2, out)
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8), out.array())
        val uv = ByteBuffer.wrap(byteArrayOf(10, 20, 11, 21))
        val u = ByteBuffer.allocate(2)
        CameraFrames.putPlane(uv, 4, 2, 2, 1, u)
        assertArrayEquals(byteArrayOf(10, 11), u.array())
    }

    @Test fun picturesAreCutDownToFit() {
        assertEquals(1280 to 960, CameraFrames.pictureSize(4000, 3000))
        assertEquals(720 to 960, CameraFrames.pictureSize(3000, 4000))
        assertEquals(640 to 480, CameraFrames.pictureSize(640, 480))
        assertEquals(2, CameraFrames.sampleSize(4000, 3000))
        assertEquals(1, CameraFrames.sampleSize(1280, 960))
        assertEquals(4, CameraFrames.sampleSize(8000, 6000))
        val (w, h) = CameraFrames.pictureSize(4000, 3000)
        assertTrue(CameraFrames.frameBytes(CameraFrames.FORMAT_RGBA, w, h) <= CameraFrames.CAPACITY)
    }

    @Test fun phoneFramesTurnUpright() {
        assertEquals(90, CameraFrames.rotation(90, 0, false))
        assertEquals(0, CameraFrames.rotation(90, 90, false))
        assertEquals(180, CameraFrames.rotation(90, 270, false))
        assertEquals(270, CameraFrames.rotation(270, 0, true))
        assertEquals(0, CameraFrames.rotation(270, 90, true))
    }

    @Test fun phoneSizePrefersFourThreeNearVga() {
        assertEquals(640 to 480, CameraFrames.phoneSize(listOf(1920 to 1080, 1280 to 720, 640 to 480, 320 to 240)))
        assertEquals(1280 to 960, CameraFrames.phoneSize(listOf(4000 to 3000, 1280 to 960, 1280 to 720)))
        assertEquals(1280 to 720, CameraFrames.phoneSize(listOf(1920 to 1080, 1280 to 720)))
        assertNull(CameraFrames.phoneSize(listOf(4000 to 3000)))
    }
}
