package com.rm.frond

import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

// The camera's host sources on Android and the frames file the core reads them from
// (native/HostCamera.h). The core scales, crops and converts; the app only hands over pixels.
object CameraFrames {
    enum class Choice(val label: String) { PATTERN("Test pattern"), PICTURE("Picture"), PHONE("Phone camera") }

    // The choices in the camera dialog, with the picker's ellipsis.
    val choices = Choice.entries
    fun choiceLabel(choice: Choice) = if (choice == Choice.PICTURE) "Picture…" else choice.label

    // What the row says: the choice, or why the core went back to the pattern.
    fun rowText(choice: Choice, coreState: String) = when {
        choice != Choice.PATTERN -> choice.label
        coreState.startsWith("pattern ") -> coreState.removePrefix("pattern ")
        else -> Choice.PATTERN.label
    }

    // The core went back to the pattern (a save, sleep, reset or leaving the app): the app's feed stops.
    fun stillFeeding(coreState: String) = coreState == "frames"

    // The file's layout: 64 header bytes, little-endian, then the pixels.
    const val HEADER_BYTES = 64
    const val CAPACITY = 1280 * 960 * 4
    const val FORMAT_RGBA = 2
    const val FORMAT_I420 = 3
    const val FLAG_MIRROR = 1
    private val MAGIC = "FrondCam".toByteArray(Charsets.US_ASCII)

    // Bytes a frame takes in the file.
    fun frameBytes(format: Int, width: Int, height: Int) = when (format) {
        FORMAT_RGBA -> width * height * 4
        FORMAT_I420 -> width * height * 3 / 2
        else -> 0
    }

    // True when the buffer holds the core's header.
    fun valid(buffer: ByteBuffer): Boolean {
        val b = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        if (b.capacity() < HEADER_BYTES + CAPACITY) return false
        for (i in MAGIC.indices) if (b.get(i) != MAGIC[i]) return false
        return b.getInt(8) == 1 && b.getInt(12) == CAPACITY
    }

    // Writes one frame: the sequence goes odd, then the fields and pixels, then even again. fill writes the
    // pixels from the buffer's position. False when the frame doesn't fit.
    fun write(buffer: ByteBuffer, format: Int, width: Int, height: Int, rotation: Int, flags: Int, fill: (ByteBuffer) -> Unit): Boolean {
        val bytes = frameBytes(format, width, height)
        if (bytes <= 0 || bytes > CAPACITY || width <= 0 || height <= 0) return false
        val b = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        val sequence = b.getInt(16)
        b.putInt(16, sequence or 1)
        fence()
        b.putInt(20, format).putInt(24, width).putInt(28, height).putInt(32, rotation).putInt(36, flags)
        b.position(HEADER_BYTES)
        fill(b)
        fence()
        b.putInt(16, (sequence or 1) + 1)
        return true
    }

    // A full barrier between the sequence and the pixels, for the core's thread, which reads the mapping
    // natively. VarHandle.fullFence from API 33. Before that a volatile write then a volatile read: the
    // write keeps the stores before it ahead (release), the read keeps the ones after it behind (acquire)
    // and the two stay in order (ART emits stlr then ldar on ARM64, a locked or fenced store on x86).
    @Volatile private var barrier = 0
    private fun fence() {
        if (android.os.Build.VERSION.SDK_INT >= 33) java.lang.invoke.VarHandle.fullFence()
        else { barrier = 0; if (barrier != 0) barrier = 0 }
    }

    // ARGB pixels (Bitmap.getPixels) as R, G, B, A bytes.
    fun putRgba(pixels: IntArray, out: ByteBuffer) {
        for (p in pixels) {
            out.put((p shr 16).toByte()); out.put((p shr 8).toByte()); out.put(p.toByte()); out.put((p ushr 24).toByte())
        }
    }

    // One YUV_420_888 plane into tight rows: rowStride and pixelStride as the Image gives them.
    fun putPlane(plane: ByteBuffer, rowStride: Int, pixelStride: Int, width: Int, height: Int, out: ByteBuffer) {
        val row = ByteArray(width)
        for (y in 0 until height) {
            val start = y * rowStride
            if (pixelStride == 1) {
                plane.position(start); plane.get(row, 0, width)
            } else for (x in 0 until width) row[x] = plane.get(start + x * pixelStride)
            out.put(row)
        }
    }

    // The largest size within 1280 x 960 a picture is cut down to, keeping its shape.
    fun pictureSize(width: Int, height: Int): Pair<Int, Int> {
        if (width <= 0 || height <= 0) return 0 to 0
        val scale = minOf(1.0, 1280.0 / width, 960.0 / height)
        return maxOf(1, (width * scale).toInt()) to maxOf(1, (height * scale).toInt())
    }

    // BitmapFactory's inSampleSize: the largest power of two that keeps the picture at least the size above.
    fun sampleSize(width: Int, height: Int): Int {
        val (w, h) = pictureSize(width, height)
        var n = 1
        while (width / (n * 2) >= w && height / (n * 2) >= h) n *= 2
        return n
    }

    // Clockwise turn for a camera frame to come out upright: the sensor's orientation less the display's
    // rotation, the other way round for a front camera.
    fun rotation(sensorDegrees: Int, displayDegrees: Int, front: Boolean) =
        if (front) (sensorDegrees + displayDegrees) % 360 else (sensorDegrees - displayDegrees + 360) % 360

    // From the camera's YUV sizes: the nearest to 640 x 480, 4:3 ones first, none above 1280 x 960.
    fun phoneSize(sizes: List<Pair<Int, Int>>): Pair<Int, Int>? {
        val fit = sizes.filter { (w, h) -> w > 0 && h > 0 && w <= 1280 && h <= 960 && w % 2 == 0 && h % 2 == 0 }
        val fourThree = fit.filter { (w, h) -> w * 3 == h * 4 }
        return (fourThree.ifEmpty { fit }).minByOrNull { (w, h) -> kotlin.math.abs(w * h - 640 * 480) }
    }

    // Sends the camera-mode requests in order on one thread and drops one whose choice a newer one
    // (another choice or leaving the app) has replaced, so a slow choice can't leave the core on it.
    // next() runs on the UI thread; send's reply goes to done on the sending thread, null when dropped.
    class Modes(private val executor: java.util.concurrent.Executor, private val request: (String, Long) -> ControlLink.Reply) {
        @Volatile var current = 0
            private set
        fun next() = ++current
        fun send(choice: Int, mode: String, timeoutSeconds: Long, done: (ControlLink.Reply?) -> Unit = {}) {
            executor.execute { done(if (choice == current) request(mode, timeoutSeconds) else null) }
        }
    }

    // The core's frames file, mapped for writing.
    class FrameFile(val path: String) : AutoCloseable {
        private val file = RandomAccessFile(java.io.File(path), "rw")
        val buffer: ByteBuffer = file.channel.map(FileChannel.MapMode.READ_WRITE, 0, (HEADER_BYTES + CAPACITY).toLong())
        val valid = valid(buffer)
        fun write(format: Int, width: Int, height: Int, rotation: Int, flags: Int, fill: (ByteBuffer) -> Unit) =
            valid && write(buffer, format, width, height, rotation, flags, fill)
        override fun close() = file.close()
    }
}
