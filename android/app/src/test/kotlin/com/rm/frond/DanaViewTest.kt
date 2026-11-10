package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class DanaViewTest {
    @get:Rule val tmp = TemporaryFolder()
    private val message = "dana-view: keyboard=auto resolved=compact zoom=2 pan-x=42.5 pan-y=12 pointer=pan density=2.75 physical=off page=symbols"

    @Test fun legacyAndInvalidPreferencesUseDefaultsWithoutDroppingValidFields() {
        val fallback = MachineSettings(danaView = DanaView(zoom = DanaZoom.THREE))
        assertEquals(fallback.danaView, MachineSettings.from(JSONObject(), fallback).danaView)
        assertEquals(DanaView(), MachineSettings.from(JSONObject()).danaView)
        // A new Dana fits the wide screen to the phone instead of starting zoomed past its width.
        assertEquals(DanaZoom.FIT, MachineSettings().danaView.zoom)
        val invalid = JSONObject().put("keyboard", "future").put("zoom", "7").put("panX", -1).put("panY", 161).put("pointer", "pan")
        assertEquals(DanaView(zoom = DanaZoom.THREE, pointer = DanaPointer.PAN), DanaView.from(invalid, fallback.danaView))
        invalid.put("panX", "NaN").put("panY", "Infinity")
        assertEquals(0.0, DanaView.from(invalid).panX, 0.0)
        assertEquals(0.0, DanaView.from(invalid).panY, 0.0)
    }

    @Test fun twoDanaMachinesRetainIndependentViewAndUnrelatedMetadata() {
        val a = File(tmp.root, "first.json")
        val b = File(tmp.root, "second.json")
        val selected = MachineSettings(danaView = DanaView(DanaKeyboard.FULL, DanaZoom.ONE_HALF, 480.5, 159.0, DanaPointer.PAN))
        for ((file, settings) in listOf(a to selected, b to MachineSettings())) {
            SessionMetadata.write(file, JSONObject().put("device", "alphasmart-dana").put("name", file.name).put("settings", settings.json()))
        }
        val notification = DanaViewState.parse(message)!!
        SessionMetadata.update(a) { json ->
            val old = MachineSettings.from(json.getJSONObject("settings"))
            json.put("settings", old.copy(danaView = notification.view).json())
        }
        val first = JSONObject(a.readText())
        assertEquals(notification.view, MachineSettings.from(first.getJSONObject("settings")).danaView)
        assertEquals("alphasmart-dana", first.getString("device"))
        assertEquals("first.json", first.getString("name"))
        assertEquals(MachineSettings(), MachineSettings.from(JSONObject(b.readText()).getJSONObject("settings")))
        val saved = selected.json().getJSONObject("danaView")
        assertFalse(saved.has("density"))
        assertFalse(saved.has("physical"))
        assertFalse(saved.has("resolved"))
        assertFalse(saved.has("page"))
        assertEquals(selected, MachineSettings.from(selected.json()))
    }

    @Test fun typedNativeStateAcceptsDiagnosticExtensionsAndRejectsMalformedState() {
        val state = DanaViewState.parse(message + " output-scale-x=1 output-scale-y=1")!!
        assertEquals(DanaKeyboard.AUTO, state.view.keyboard)
        assertEquals(DanaKeyboard.COMPACT, state.resolved)
        assertEquals(42.5, state.view.panX, 0.0)
        assertEquals(2.75, state.density, 0.0)
        assertEquals("symbols", state.page)
        for (bad in listOf(
            message.removePrefix("dana-view:"), message + " keyboard=full", message + " page=fn",
            message.replace("pan-x=42.5", "pan-x=NaN"), message.replace("pan-y=12", "pan-y=161"),
            message.replace("resolved=compact", "resolved=auto"), message.replace("density=2.75", "density=0"),
            message.replace("physical=off", "physical=maybe"), message.replace(" zoom=2", ""),
            message.replace("page=symbols", "page=unknown"), message + " invalid",
        )) assertNull(bad, DanaViewState.parse(bad))
    }

    @Test fun hardwareDetectionExcludesVirtualNonAlphabeticAndDisabledDevices() {
        val physical = DanaKeyboardDevice(virtual = false, alphabetic = true, enabled = true)
        assertTrue(hasDanaHardwareKeyboard(listOf(physical)))
        assertFalse(hasDanaHardwareKeyboard(emptyList()))
        assertFalse(hasDanaHardwareKeyboard(listOf(physical.copy(virtual = true), physical.copy(alphabetic = false), physical.copy(enabled = false))))
        // Hardware updates carry no user fields, so explicit full/compact/hidden choices remain native overrides.
        for (mode in DanaKeyboard.entries) {
            val view = DanaView(keyboard = mode)
            assertFalse(danaEnvironment(3f, true).contains("keyboard="))
            assertEquals(mode, DanaView.from(view.json()).keyboard)
        }
    }

    @Test fun quickShowOverridesAutomaticHardwareHidingAndRestoresAnExplicitMode() {
        val automatic = DanaViewState.parse(message.replace("resolved=compact", "resolved=hidden").replace("physical=off", "physical=on"))!!
        assertEquals(DanaKeyboard.COMPACT, automatic.toggledKeyboard(DanaKeyboard.AUTO))
        assertEquals(DanaKeyboard.FULL, automatic.toggledKeyboard(DanaKeyboard.FULL))
        assertEquals(DanaKeyboard.AUTO, automatic.copy(physical = false).toggledKeyboard(DanaKeyboard.AUTO))
        assertEquals(DanaKeyboard.HIDDEN, automatic.copy(resolved = DanaKeyboard.FULL).toggledKeyboard(DanaKeyboard.FULL))
    }

    @Test fun danaControlsAreSelectedByIdAndDoNotChangeOtherDevices() {
        assertTrue(Device.TX.copy(id = "alphasmart-dana").isDana())
        assertFalse(Device.TX.isDana())
        val other = MachineSettings(danaView = DanaView(keyboard = DanaKeyboard.HIDDEN))
        assertFalse("--dana-view" in other.arguments())
        assertEquals("fit=fit smooth=off", other.displayArgument())
    }

    @Test
    fun wirelessGetsTheDanaViewAndFit() {
        // The Wireless takes the Dana's view, keyboard and Fit zoom, from a new machine and from an imported one.
        for (id in listOf("alphasmart-dana", "alphasmart-dana-wireless")) {
            val device = Device.byId(id)!!
            assertTrue(id, device.isDana())
            assertEquals(id, 560 to 160, device.width to device.height)
        }
        assertEquals(DanaZoom.FIT, MachineSettings().danaView.zoom)
        assertEquals(DanaZoom.FIT, MachineSettings.from(JSONObject()).danaView.zoom)
        assertEquals(DanaZoom.FIT, MachineSettings.from(JSONObject().put("danaView", JSONObject())).danaView.zoom)
    }
}
