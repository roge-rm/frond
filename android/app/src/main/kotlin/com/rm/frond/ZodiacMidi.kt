package com.rm.frond

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.IOException

object ZodiacMidiChoice {
    // The first input port (the one a device takes music on) from (type, number) pairs, or null.
    fun inputPort(ports: List<Pair<Int, Int>>): Int? =
        ports.firstOrNull { it.first == MidiDeviceInfo.PortInfo.TYPE_INPUT }?.second

    // The core flag for a chosen MidiManager device, or none.
    fun arguments(deviceId: Int?): List<String> =
        deviceId?.let { listOf("--music-midi-out", it.toString()) }.orEmpty()
}

// The Zodiac's music to a MIDI device through MidiManager: USB devices and Bluetooth MIDI devices
// already paired. The core's MIDI out thread calls send with each message and the System.nanoTime
// it's due; messages before the device opens are dropped.
class ZodiacMidi(context: Context, deviceId: Int) {
    @Volatile private var port: MidiInputPort? = null
    private var device: MidiDevice? = null
    private var closed = false
    private val lock = Any()

    init {
        val manager = context.getSystemService(Context.MIDI_SERVICE) as? MidiManager
        @Suppress("DEPRECATION")
        val info = manager?.devices?.firstOrNull { it.id == deviceId }
        val number = info?.let { i -> ZodiacMidiChoice.inputPort(i.ports.map { it.type to it.portNumber }) }
        if (manager == null || info == null || number == null) {
            Log.w(FrondActivity.TAG, "MIDI out: no device $deviceId with an input port")
        } else {
            manager.openDevice(info, { opened ->
                if (opened == null) {
                    Log.w(FrondActivity.TAG, "MIDI out: couldn't open device $deviceId")
                    return@openDevice
                }
                synchronized(lock) {
                    if (closed) {
                        opened.close()
                        return@openDevice
                    }
                    device = opened
                    port = opened.openInputPort(number)
                }
                Log.i(FrondActivity.TAG, "MIDI out: ${info.properties.getString(MidiDeviceInfo.PROPERTY_NAME)} port $number")
            }, Handler(Looper.getMainLooper()))
        }
    }

    // Any thread.
    fun send(bytes: ByteArray, timestampNs: Long) {
        try {
            port?.send(bytes, 0, bytes.size, timestampNs)
        } catch (e: IOException) {
            // A device that went away: the music stops, the machine goes on.
        } catch (e: IllegalStateException) {
            // Closed meanwhile.
        }
    }

    fun close() = synchronized(lock) {
        closed = true
        port?.close()
        port = null
        device?.close()
        device = null
    }
}
