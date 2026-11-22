package com.rm.frond

import android.media.midi.MidiDeviceInfo.PortInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ZodiacMidiTest {
    @Test fun firstInputPort() {
        assertEquals(2, ZodiacMidiChoice.inputPort(listOf(PortInfo.TYPE_OUTPUT to 0, PortInfo.TYPE_INPUT to 2, PortInfo.TYPE_INPUT to 3)))
        assertNull(ZodiacMidiChoice.inputPort(listOf(PortInfo.TYPE_OUTPUT to 0)))
        assertNull(ZodiacMidiChoice.inputPort(emptyList()))
    }

    @Test fun deviceByName() {
        val devices = listOf(1 to "Synth", 4 to "Keys", 9 to "Synth")
        assertEquals(1, ZodiacMidiChoice.deviceId(devices, "Synth"))
        assertEquals(4, ZodiacMidiChoice.deviceId(devices, "Keys"))
        assertNull(ZodiacMidiChoice.deviceId(devices, "Gone"))
        assertNull(ZodiacMidiChoice.deviceId(devices, ""))
    }

    @Test fun musicSettingRoundTripsAndGivesTheCoreFlags() {
        val fm = MachineSettings()
        assertFalse(fm.json().has("musicOutput"))
        assertEquals(MusicOutput.FM, MachineSettings.from(fm.json()).musicOutput)
        val font = MachineSettings(musicOutput = MusicOutput.SOUNDFONT, soundFont = "/data/soundfonts/GM.sf2", midiDevice = "Synth")
        assertEquals(font, MachineSettings.from(font.json()))
        assertEquals(listOf("--music-soundfont", "/data/soundfonts/GM.sf2"), font.musicArguments(true, 3))
        // Gone or nothing picked: the game's own sound.
        assertEquals(emptyList<String>(), font.musicArguments(false, 3))
        assertEquals(emptyList<String>(), font.copy(soundFont = "").musicArguments(true, null))
        val midi = font.copy(musicOutput = MusicOutput.MIDI)
        assertEquals(listOf("--music-midi-out", "3"), midi.musicArguments(true, 3))
        assertEquals(emptyList<String>(), midi.musicArguments(true, null))
        assertEquals(emptyList<String>(), fm.musicArguments(true, 3))
        assertEquals(MusicOutput.FM, MachineSettings.from(font.json().put("musicOutput", "radio")).musicOutput)
    }

    @Test fun soundFontFiles() {
        val head = "RIFF\u0000\u0001\u0000\u0000sfbkLIST".toByteArray(Charsets.ISO_8859_1)
        assertTrue(SoundFonts.isSoundFont(head))
        assertFalse(SoundFonts.isSoundFont("RIFF\u0000\u0001\u0000\u0000WAVEfmt ".toByteArray(Charsets.ISO_8859_1)))
        assertFalse(SoundFonts.isSoundFont(head.copyOf(8)))
        assertEquals("GM.sf2", SoundFonts.cleanName("GM.sf2"))
        assertEquals("Piano.sf2", SoundFonts.cleanName("Piano"))
        assertEquals("evil.sf2", SoundFonts.cleanName("../../.evil.sf2"))
        assertEquals("SoundFont.sf2", SoundFonts.cleanName(null))
    }

    @Test fun flagOnlyWithADevice() {
        assertEquals(listOf("--music-midi-out", "7"), ZodiacMidiChoice.arguments(7))
        assertEquals(emptyList<String>(), ZodiacMidiChoice.arguments(null))
    }
}
