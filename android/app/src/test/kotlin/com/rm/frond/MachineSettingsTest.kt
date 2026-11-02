package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class MachineSettingsTest {
    @get:Rule val tmp = TemporaryFolder()

    private val legacy = MachineSettings(ScreenSize.WHOLE, smoothing = true, sound = false, fastAudio = true)

    private fun metadata(id: String, settings: MachineSettings? = null): File {
        val file = File(tmp.root, "$id.json")
        val json = JSONObject().put("name", id).put("device", Device.TX.id).put("created", 1L).put("used", 0L)
        settings?.let { json.put("settings", it.json()) }
        SessionMetadata.write(file, json)
        return file
    }

    private fun settings(file: File) = MachineSettings.from(SessionMetadata.read(file) { legacy }.optJSONObject("settings"))

    @Test fun independentControlVisibilityDefaultsAndRoundTrips() {
        val old = MachineSettings.from(
            legacy.json().apply { remove("showKeyboard"); remove("showJogControls") },
            MachineSettings(showKeyboard = false, showJogControls = false),
        )
        assertTrue(old.showKeyboard)
        assertTrue(old.showJogControls)
        val first = metadata("visibility-first", legacy.copy(microphone = true))
        val second = metadata("visibility-second", legacy)
        for (keyboard in listOf(false, true)) for (jog in listOf(false, true)) {
            val chosen = legacy.copy(microphone = true, showKeyboard = keyboard, showJogControls = jog)
            SessionMetadata.update(first) { it.put("settings", chosen.json()) }
            val loaded = settings(first)
            assertEquals(chosen, loaded)
            assertEquals(legacy, settings(second))
            val arguments = loaded.arguments(tg50Controls = true)
            assertEquals(!keyboard, "--hide-keyboard" in arguments)
            assertEquals(!jog, "--hide-jog-controls" in arguments)
            assertFalse("--hide-keyboard" in loaded.arguments())
            assertFalse("--hide-jog-controls" in loaded.arguments())
        }
    }

    @Test fun unsupportedSoundMutesWithoutChangingTheSavedChoice() {
        val chosen = MachineSettings(sound = true)
        assertTrue("--no-sound" in chosen.arguments(soundSupported = false))
        assertFalse("--no-sound" in chosen.arguments(soundSupported = true))
        assertTrue(chosen.sound)
        assertTrue(MachineSettings.from(chosen.json()).sound)
        assertTrue("--no-sound" in chosen.copy(sound = false).arguments(soundSupported = true))
    }

    @Test fun oldMachineKeepsAllLegacyChoicesAfterReload() {
        val file = metadata("old")
        assertEquals(legacy, settings(file))
        val restarted = SessionMetadata.read(file) { error("migration should already be saved") }
        assertEquals(legacy, MachineSettings.from(restarted.getJSONObject("settings")))
        assertEquals("old", restarted.getString("name"))
    }

    @Test fun newMachineUsesDefaultsAndRoundTripsEveryPreference() {
        val file = metadata("new", MachineSettings())
        assertEquals(MachineSettings(), settings(file))
        SessionMetadata.update(file) { it.put("settings", legacy.json()) }
        assertEquals(legacy, settings(file))
    }

    @Test fun twoMachinesOfTheSameModelRemainIndependent() {
        val first = metadata("first")
        val second = metadata("second")
        assertEquals(legacy, settings(first))
        assertEquals(legacy, settings(second))
        val changed = MachineSettings(ScreenSize.FILL, sound = true)
        SessionMetadata.update(first) { it.put("settings", changed.json()) }
        assertEquals(changed, settings(first))
        assertEquals(legacy, settings(second))
    }

    @Test fun renameAndLaunchTimeKeepTheLatestLiveSound() {
        val file = metadata("machine", legacy)
        val stale = settings(file)
        SessionMetadata.update(file) { json ->
            val current = MachineSettings.from(json.getJSONObject("settings"))
            json.put("settings", current.copy(sound = true).json())
        }
        SessionMetadata.update(file) { it.put("name", "Renamed") }
        SessionMetadata.update(file) { it.put("used", 42L) }
        assertFalse(stale.sound)
        assertEquals(legacy.copy(sound = true), settings(file))
        val current = JSONObject(file.readText())
        assertEquals("Renamed", current.getString("name"))
        assertEquals(42L, current.getLong("used"))
        assertFalse(File(file.path + ".tmp").exists())
    }

    @Test fun unknownSizeUsesFallbackWithoutDroppingOtherPreferences() {
        val parsed = MachineSettings.from(JSONObject().put("size", "future").put("sound", false))
        assertEquals(ScreenSize.FIT, parsed.size)
        assertFalse(parsed.sound)
        assertFalse(parsed.fastAudio)
    }

    @Test fun deletedMachineCannotBeRecreatedByStaleUpdate() {
        val file = metadata("machine", legacy)
        assertTrue(file.delete())
        assertThrows(Exception::class.java) { SessionMetadata.update(file) { it.put("used", 2L) } }
        assertFalse(file.exists())
    }

    @Test fun concurrentMetadataChangesDoNotLoseUpdates() {
        val file = metadata("machine", legacy)
        val start = CountDownLatch(1)
        val pool = Executors.newFixedThreadPool(2)
        try {
            val jobs = (0..1).map {
                pool.submit {
                    start.await()
                    repeat(10) { SessionMetadata.update(file) { json -> json.put("used", json.getLong("used") + 1) } }
                }
            }
            start.countDown()
            jobs.forEach { it.get(10, TimeUnit.SECONDS) }
            assertEquals(20L, JSONObject(file.readText()).getLong("used"))
            assertEquals(legacy, settings(file))
        } finally {
            pool.shutdownNow()
        }
    }

    @Test fun launchArgumentsFollowThisMachineAfterReload() {
        val file = metadata("machine", legacy)
        assertEquals(listOf("--window-fit", "whole", "--smooth-lcd", "--no-sound"), settings(file).arguments())
        val other = metadata("other", MachineSettings(ScreenSize.FILL))
        assertEquals(listOf("--window-fit", "fill"), settings(other).arguments())
        val global = Settings(orientation = Orientation.LANDSCAPE, debugger = true, debuggerPort = 5555)
        val launch = settings(file).applyTo(global)
        assertEquals(Orientation.LANDSCAPE, launch.orientation)
        assertTrue(launch.debugger)
        assertEquals(5555, launch.debuggerPort)
        assertEquals(ScreenSize.WHOLE, launch.size)
        assertFalse(launch.sound)
    }
}
