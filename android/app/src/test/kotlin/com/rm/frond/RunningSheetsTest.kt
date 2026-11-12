package com.rm.frond

import com.rm.frond.RunningSheets.AppAction
import com.rm.frond.RunningSheets.AudioRow
import com.rm.frond.RunningSheets.CardIcon
import com.rm.frond.RunningSheets.CardRow
import com.rm.frond.RunningSheets.ConnRow
import com.rm.frond.RunningSheets.DisplayRow
import com.rm.frond.RunningSheets.Sheet
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class RunningSheetsTest {
    private val off = NetRelay.Phase.OFF
    private val gone = NetRelay.Phase.DISCONNECTED

    // Storage is on every machine, with Internal even where there's no slot.
    @Test fun everyMachineHasStorage() {
        for (device in Device.entries) {
            assertEquals(device.id, listOf(Sheet.APPLICATIONS, Sheet.CARD), RunningSheets.barSheets(device))
            assertEquals("Storage", RunningSheets.title(Sheet.CARD, device))
        }
        val slotless = Device.entries.first { !it.cardSupported }
        assertTrue(RunningSheets.cardRows(slotless, SdCard.State.NONE, false, false).isEmpty())
    }

    @Test fun menuRowsOpenTheirSheets() {
        assertEquals(Sheet.DISPLAY, RunningSheets.menuSheet(RunningMenu.Id.DISPLAY))
        assertEquals(Sheet.CONNECTIONS, RunningSheets.menuSheet(RunningMenu.Id.CONNECTIONS))
        assertNull(RunningSheets.menuSheet(RunningMenu.Id.RESET))
    }

    @Test fun appsGetLaunchAndExportOnly() {
        assertEquals(listOf(AppAction.LAUNCH, AppAction.EXPORT, AppAction.DELETE), RunningSheets.appActions)
        val list = listOf(
            PalmDatabases.Info("Memos", "appl", "memo", true),
            PalmDatabases.Info("MemoDB", "DATA", "memo", false),
            PalmDatabases.Info("Hacks", "DATA", "hack", true),
        )
        assertEquals(listOf("Memos"), RunningSheets.applications(list).map { it.name })
    }

    @Test fun cardIconShowsTheState() {
        assertEquals(CardIcon.IN, RunningSheets.cardIcon(SdCard.State.INSERTED, false))
        assertEquals(CardIcon.IN, RunningSheets.cardIcon(SdCard.State.INSERTED, true))
        assertEquals(CardIcon.EJECTED, RunningSheets.cardIcon(SdCard.State.EJECTED, false))
        assertEquals(CardIcon.OUT, RunningSheets.cardIcon(SdCard.State.NONE, true))
        assertEquals(CardIcon.NONE, RunningSheets.cardIcon(SdCard.State.NONE, false))
        assertEquals(CardIcon.NONE, RunningSheets.cardIcon(SdCard.State.UNKNOWN, true))
    }

    @Test fun cardRowsFollowTheState() {
        val sd = Device.entries.first { it.sdSlot && !it.memoryStick }
        fun rows(state: SdCard.State, linked: Boolean = false, free: Boolean = false) = RunningSheets.cardRows(sd, state, linked, free)
        assertEquals(listOf(CardRow.SLOT, CardRow.FILES, CardRow.EJECT), rows(SdCard.State.INSERTED))
        assertEquals(listOf(CardRow.SLOT, CardRow.INSERT, CardRow.FILES, CardRow.EXPORT_IMAGE), rows(SdCard.State.EJECTED))
        assertEquals(listOf(CardRow.SLOT), rows(SdCard.State.NONE))
        assertEquals(listOf(CardRow.SLOT, CardRow.LINKED), rows(SdCard.State.NONE, linked = true))
        assertEquals(listOf(CardRow.SLOT, CardRow.LINKED, CardRow.INSERT_LINKED), rows(SdCard.State.NONE, linked = true, free = true))
        assertEquals(listOf(CardRow.SLOT, CardRow.TRY_AGAIN), rows(SdCard.State.UNKNOWN))
        assertFalse(CardRow.WRITE_PROTECT in rows(SdCard.State.EJECTED))
    }

    @Test fun writeProtectIsForMemorySticksWithACard() {
        val stick = Device.entries.firstOrNull { it.memoryStick } ?: return
        assertTrue(CardRow.WRITE_PROTECT in RunningSheets.cardRows(stick, SdCard.State.EJECTED, false, false))
        assertTrue(CardRow.WRITE_PROTECT in RunningSheets.cardRows(stick, SdCard.State.INSERTED, false, false))
        assertFalse(CardRow.WRITE_PROTECT in RunningSheets.cardRows(stick, SdCard.State.NONE, false, false))
        // Files… works on an ejected stick the same as on an SD card.
        assertTrue(CardRow.FILES in RunningSheets.cardRows(stick, SdCard.State.EJECTED, false, false))
    }

    @Test fun noCardSlotNoRows() {
        val none = Device.entries.first { !it.cardSupported }
        assertTrue(RunningSheets.cardRows(none, SdCard.State.INSERTED, false, false).isEmpty())
    }

    @Test fun slotOpensThePickerOnlyWhenIdle() {
        assertTrue(RunningSheets.slotUsable(SdCard.State.INSERTED, busy = false, pending = false))
        assertFalse(RunningSheets.slotUsable(SdCard.State.UNKNOWN, false, false))
        assertFalse(RunningSheets.slotUsable(SdCard.State.NONE, true, false))
        assertFalse(RunningSheets.slotUsable(SdCard.State.NONE, false, true))
    }

    @Test fun rotateIsForArmDevicesAndSpeedFor68k() {
        for (device in Device.entries) {
            val rows = RunningSheets.displayRows(device)
            assertEquals(device.id, !device.runs68k, DisplayRow.ROTATE in rows)
            assertEquals(device.id, device.runs68k, DisplayRow.SPEED in rows)
            assertEquals(device.id, device.lcdContrast, DisplayRow.CONTRAST in rows)
            assertEquals(device.id, device.id == "sony-clie-tg50", DisplayRow.SHOW_KEYBOARD in rows && DisplayRow.JOG in rows)
            assertTrue(DisplayRow.SIZE in rows && DisplayRow.SMOOTH in rows)
        }
    }

    @Test fun jogControlsFollowTheSideTab() {
        val tg50 = Device.byId("sony-clie-tg50")!!
        assertTrue(tg50.jogControls && !tg50.jogBesideButtons)
        // A CLIE with a side tab and a silkscreen, like the T415: Jog controls, no Show keyboard.
        val clie = Device.byId("palm-m515")!!.copy(id = "sony-clie-t415", sideTabWidth = 38, silkscreenHeight = 128)
        assertTrue(clie.jogControls && clie.jogBesideButtons)
        val rows = RunningSheets.displayRows(clie)
        assertTrue(DisplayRow.JOG in rows && DisplayRow.SHOW_KEYBOARD !in rows)
        for (device in Device.entries) assertEquals(device.id, device.sideTabWidth > 0, device.jogControls)
    }

    @Test fun audioRowsFollowTheHardware() {
        for (device in Device.entries) {
            val rows = RunningSheets.audioRows(device)
            assertEquals(AudioRow.SOUND, rows.first())
            assertEquals(device.id, !device.runs68k || device.microphone, AudioRow.MICROPHONE in rows)
            assertEquals(device.id, !device.runs68k, AudioRow.AUDIO_PREVIEW in rows)
        }
    }

    @Test fun connectionRowsFollowTheDevice() {
        for (device in Device.entries) {
            val rows = RunningSheets.connectionRows(device, off, false)
            assertEquals(ConnRow.BEAM, rows.first())
            assertEquals(device.id, device.hotSyncButton, ConnRow.HOTSYNC in rows)
            assertEquals(device.id, !device.serialAbsent, ConnRow.SERIAL in rows)
            assertEquals(device.id, !device.runs68k || device.usb, ConnRow.USB in rows)
            assertEquals(device.id, !device.runs68k || device.bluetooth, ConnRow.BLUETOOTH in rows)
            assertEquals(device.id, device.camera, ConnRow.CAMERA in rows)
            assertEquals(device.id, device.usbPrinter || device.usbWifi, ConnRow.USB_PORT in rows)
            assertEquals(device.id, device.builtInWifi, ConnRow.WIFI_MODULE in rows)
            assertFalse(ConnRow.DEBUGGER in rows)
            assertFalse(ConnRow.USB_RETRY in rows)
        }
    }

    @Test fun retryAndDebuggerRowsAppearWhenNeeded() {
        val device = Device.entries.first { it.serialPort && it.usb }
        val rows = RunningSheets.connectionRows(device, gone, true)
        assertTrue(ConnRow.USB_RETRY in rows && ConnRow.DEBUGGER in rows)
        // Infrared and serial over a connection reconnect by themselves, so they have no retry row.
        assertEquals(ConnRow.BEAM, rows[0])
    }

    @Test fun beamingNeedsTheKeyboardOff() {
        val device = Device.entries.first { it.infrared }
        assertTrue(RunningSheets.beamUsable(device, false))
        assertFalse(RunningSheets.beamUsable(device, true))
        assertFalse(RunningSheets.beamUsable(device, null))
    }

    @Test fun byNameDialogTakesCoreNames() {
        assertTrue(RunningSheets.validDatabaseName(" Memos-PMem "))
        assertTrue(RunningSheets.validDatabaseName("a".repeat(31)))
        assertFalse(RunningSheets.validDatabaseName("a".repeat(32)))
        assertFalse(RunningSheets.validDatabaseName("  "))
        val all = listOf(PalmDatabases.Info("Memos-PMem", "DATA", "memo", false), PalmDatabases.Info("Todo", "appl", "todo", true))
        assertEquals(listOf("Todo"), RunningSheets.matching(all, " tod ").map { it.name })
        assertEquals(2, RunningSheets.matching(all, "").size)
    }
}
