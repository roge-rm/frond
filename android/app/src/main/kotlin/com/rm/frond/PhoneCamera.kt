package com.rm.frond

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Log

// The phone's back camera through Camera2, writing each frame as I420 into the core's frames file.
// start and stop run on the UI thread; frames arrive on the camera's own thread. The caller holds the
// CAMERA permission. failed is called once, on the camera thread, when the camera can't run.
class PhoneCamera(
    private val context: Context,
    private val frames: CameraFrames.FrameFile,
    private val displayDegrees: () -> Int,
    private val failed: (String) -> Unit,
) {
    private var thread: HandlerThread? = null
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    @Volatile private var stopped = false

    @SuppressLint("MissingPermission")
    fun start() {
        val manager = context.getSystemService(CameraManager::class.java) ?: return failed("No camera")
        val id = try {
            manager.cameraIdList.firstOrNull { manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK }
                ?: manager.cameraIdList.firstOrNull()
        } catch (e: Exception) { null } ?: return failed("No camera")
        val characteristics = manager.getCameraCharacteristics(id)
        val front = characteristics.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        val sensor = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        val sizes = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(ImageFormat.YUV_420_888)?.map { it.width to it.height }.orEmpty()
        val (width, height) = CameraFrames.phoneSize(sizes) ?: return failed("No camera size fits")
        val t = HandlerThread("FrondCamera").also { it.start() }
        thread = t
        val handler = Handler(t.looper)
        val r = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 2)
        reader = r
        r.setOnImageAvailableListener({ source ->
            val image = try { source.acquireLatestImage() } catch (e: IllegalStateException) { null } ?: return@setOnImageAvailableListener
            try {
                if (stopped) return@setOnImageAvailableListener
                val planes = image.planes
                val rotation = CameraFrames.rotation(sensor, displayDegrees(), front)
                frames.write(CameraFrames.FORMAT_I420, width, height, rotation, if (front) CameraFrames.FLAG_MIRROR else 0) { out ->
                    CameraFrames.putPlane(planes[0].buffer, planes[0].rowStride, planes[0].pixelStride, width, height, out)
                    for (i in 1..2) CameraFrames.putPlane(planes[i].buffer, planes[i].rowStride, planes[i].pixelStride, width / 2, height / 2, out)
                }
            } finally {
                image.close()
            }
        }, handler)
        try {
            manager.openCamera(id, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    if (stopped) { camera.close(); return }
                    device = camera
                    @Suppress("DEPRECATION")
                    camera.createCaptureSession(listOf(r.surface), object : CameraCaptureSession.StateCallback() {
                        override fun onConfigured(s: CameraCaptureSession) {
                            if (stopped) { s.close(); return }
                            session = s
                            try {
                                val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply { addTarget(r.surface) }.build()
                                s.setRepeatingRequest(request, null, handler)
                            } catch (e: Exception) { fail("Couldn't start the camera", e) }
                        }
                        override fun onConfigureFailed(s: CameraCaptureSession) = fail("Couldn't start the camera", null)
                    }, handler)
                }
                override fun onDisconnected(camera: CameraDevice) { camera.close(); fail("The camera was taken by another app", null) }
                override fun onError(camera: CameraDevice, error: Int) { camera.close(); fail("Camera error $error", null) }
            }, handler)
        } catch (e: Exception) {
            fail("Couldn't open the camera", e)
        }
    }

    fun stop() {
        stopped = true
        val t = thread ?: return
        thread = null
        // Closed on the camera thread, after anything queued there.
        Handler(t.looper).post {
            try { session?.close() } catch (_: Exception) {}
            try { device?.close() } catch (_: Exception) {}
            reader?.close()
            session = null; device = null; reader = null
            t.quitSafely()
        }
    }

    private fun fail(why: String, e: Exception?) {
        if (stopped) return
        Log.w("Frond", "camera: $why", e)
        failed(why)
    }
}
