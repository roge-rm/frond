package com.rm.frond

import com.rm.frond.RunningSheets.AppAction
import com.rm.frond.RunningSheets.AudioRow
import com.rm.frond.RunningSheets.CardIcon
import com.rm.frond.RunningSheets.CardRow
import com.rm.frond.RunningSheets.ConnRow
import com.rm.frond.RunningSheets.DisplayRow
import com.rm.frond.RunningSheets.Sheet
import com.rm.frond.RunningSheets.ZaurusUsbMode
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

    // The settings sheet groups follow the desktop's: Display, Audio, Game controller, Battery, Speed.
    @Test fun settingsGroupsFollowTheDesktop() {
        for (device in Device.entries) {
            val groups = RunningSheets.groups(device)
            assertEquals(device.id, groups.sortedBy { it.ordinal }, groups)
            assertEquals(listOf(RunningSheets.Group.DISPLAY, RunningSheets.Group.AUDIO), groups.take(2))
            assertTrue(device.id, RunningSheets.Group.BATTERY in groups)
            assertEquals(device.id, RunningSheets.controlsShown(device), RunningSheets.Group.GAME_CONTROLLER in groups)
        }
        assertEquals("Settings", RunningSheets.title(Sheet.DISPLAY, Device.entries.first()))
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
        val sd = Device.entries.first { it.sdSlot && !it.memoryStick && !it.sdWriteProtection }
        fun rows(state: SdCard.State, linked: Boolean = false, free: Boolean = false) = RunningSheets.cardRows(sd, state, linked, free)
        assertEquals(listOf(CardRow.SLOT, CardRow.FILES, CardRow.EJECT), rows(SdCard.State.INSERTED))
        assertEquals(listOf(CardRow.SLOT, CardRow.INSERT, CardRow.FILES, CardRow.EXPORT_IMAGE), rows(SdCard.State.EJECTED))
        assertEquals(listOf(CardRow.SLOT), rows(SdCard.State.NONE))
        assertEquals(listOf(CardRow.SLOT, CardRow.LINKED), rows(SdCard.State.NONE, linked = true))
        assertEquals(listOf(CardRow.SLOT, CardRow.LINKED, CardRow.INSERT_LINKED), rows(SdCard.State.NONE, linked = true, free = true))
        assertEquals(listOf(CardRow.SLOT, CardRow.TRY_AGAIN), rows(SdCard.State.UNKNOWN))
        assertFalse(CardRow.WRITE_PROTECT in rows(SdCard.State.EJECTED))
    }

    @Test fun unlinkShowsForALinkedSlotInAnyState() {
        val sd = Device.entries.first { it.sdSlot && !it.memoryStick && !it.sdWriteProtection }
        fun rows(state: SdCard.State, unlinkable: Boolean = true, linked: Boolean = false) =
            RunningSheets.cardRows(sd, state, linked, false, unlinkable = unlinkable)
        assertEquals(listOf(CardRow.SLOT, CardRow.LINKED, CardRow.UNLINK), rows(SdCard.State.NONE, linked = true))
        assertEquals(listOf(CardRow.SLOT, CardRow.FILES, CardRow.EJECT, CardRow.UNLINK), rows(SdCard.State.INSERTED))
        assertEquals(listOf(CardRow.SLOT, CardRow.INSERT, CardRow.FILES, CardRow.EXPORT_IMAGE, CardRow.UNLINK), rows(SdCard.State.EJECTED))
        assertFalse(CardRow.UNLINK in rows(SdCard.State.INSERTED, unlinkable = false))
        assertFalse(CardRow.UNLINK in rows(SdCard.State.UNKNOWN))
        assertFalse(CardRow.UNLINK in RunningSheets.cardRows(sd, SdCard.State.INSERTED, false, false, oldPlace = true, unlinkable = true))
    }

    @Test fun setControlsShowsWhereThereAreButtonsToSet() {
        for (device in Device.entries) assertEquals(device.id, PalmButton.listed(device).isNotEmpty(), RunningSheets.controlsShown(device))
        assertEquals("Controls", RunningSheets.title(Sheet.CONTROLS, Device.TX))
    }

    @Test fun palmWifiSharesThePrimaryMemoryCardSlot() {
        val supported = setOf("palm-e2", "palm-e2-franken", "palm-zire-72")
        for (device in Device.entries) {
            assertEquals(device.id, device.id in supported, device.palmWifiCard)
        }
        val device = Device.E2
        fun rows(state: SdCard.State) = RunningSheets.cardRows(device, state, false, false)
        assertTrue(CardRow.INSERT_PALM_WIFI in rows(SdCard.State.NONE))
        assertTrue(CardRow.INSERT_PALM_WIFI in rows(SdCard.State.EJECTED))
        assertFalse(CardRow.INSERT_PALM_WIFI in rows(SdCard.State.INSERTED))
        for (reply in listOf("inserted wifi", "inserted wifi ejected")) {
            val restored = SdCard.parseState(reply, m68k = false)
            assertEquals(SdCard.State.PALM_WIFI, restored)
            assertEquals(listOf(CardRow.SLOT, CardRow.EJECT_PALM_WIFI), rows(restored))
            assertEquals(CardIcon.IN, RunningSheets.cardIcon(restored, false))
            assertFalse(RunningSheets.slotUsable(restored, busy = false, pending = false))
        }
        assertEquals(SdCard.State.INSERTED, SdCard.parseState("inserted key writable", m68k = true))
        assertTrue(device.networkSupported)
        assertEquals(listOf(NetworkMode.DISABLED, NetworkMode.WIRELESS), NetworkMode.offered(device.networkWifiOnly))
    }

    @Test fun writeProtectIsForMemorySticksWithACard() {
        val stick = Device.entries.firstOrNull { it.memoryStick } ?: return
        assertTrue(CardRow.WRITE_PROTECT in RunningSheets.cardRows(stick, SdCard.State.EJECTED, false, false))
        assertTrue(CardRow.WRITE_PROTECT in RunningSheets.cardRows(stick, SdCard.State.INSERTED, false, false))
        assertFalse(CardRow.WRITE_PROTECT in RunningSheets.cardRows(stick, SdCard.State.NONE, false, false))
        // Files… works on an ejected stick the same as on an SD card.
        assertTrue(CardRow.FILES in RunningSheets.cardRows(stick, SdCard.State.EJECTED, false, false))
    }

    @Test fun headphoneAndWriteProtectControlsFollowTheBoard() {
        val armPalms = setOf("palm-tx", "palm-e2", "palm-e2-franken", "palm-zire-72")
        for (device in Device.entries) {
            val supported = device.id in armPalms
            assertEquals(device.id, supported || device.zaurus, AudioRow.HEADPHONES in RunningSheets.audioRows(device))
            assertEquals(device.id, supported || device.zodiac || device.zaurus, device.sdWriteProtection)
            if (supported) {
                val card = RunningSheets.cardRows(device, SdCard.State.EJECTED, false, false)
                assertTrue(device.id, CardRow.WRITE_PROTECT in card)
            }
        }
    }

    @Test fun zaurusHasSeparateCardRowsAndOpensFilesOnlyAfterEjecting() {
        val zaurus = Device.byId(Device.ZAURUS_ID)!!
        fun rows(n: Int, state: SdCard.State) = RunningSheets.cardRows(zaurus, state, false, false, n)
        assertTrue(zaurus.sdSlot)
        assertFalse(CardRow.IMPORT_CF_IMAGE in rows(0, SdCard.State.NONE))
        assertTrue(CardRow.IMPORT_CF_IMAGE in rows(1, SdCard.State.NONE))
        for (n in 0..1) {
            assertFalse(CardRow.FILES in rows(n, SdCard.State.INSERTED))
            assertTrue(CardRow.EJECT in rows(n, SdCard.State.INSERTED))
            assertTrue(CardRow.FILES in rows(n, SdCard.State.EJECTED))
            assertTrue(CardRow.WRITE_PROTECT in rows(n, SdCard.State.EJECTED))
        }
    }

    @Test fun bothZodiacSdSlotsShowWriteProtection() {
        val zodiac = Device.byId("tapwave-zodiac-2")!!
        for (n in 0..1) {
            assertTrue(CardRow.WRITE_PROTECT in RunningSheets.cardRows(zodiac, SdCard.State.EJECTED, false, false, n))
            assertTrue(CardRow.WRITE_PROTECT in RunningSheets.cardRows(zodiac, SdCard.State.INSERTED, false, false, n))
            assertFalse(CardRow.WRITE_PROTECT in RunningSheets.cardRows(zodiac, SdCard.State.NONE, false, false, n))
        }
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

    @Test fun rotateIsForArmDevicesAndSpeedForAll() {
        for (device in Device.entries) {
            val rows = RunningSheets.displayRows(device)
            assertTrue(device.id, DisplayRow.SPEED in rows)
            assertEquals(device.id, device.lcdContrast, DisplayRow.CONTRAST in rows)
            assertEquals(device.id, device.id == "sony-clie-tg50", DisplayRow.SHOW_KEYBOARD in rows && DisplayRow.JOG in rows)
            assertTrue(DisplayRow.SIZE in rows && DisplayRow.SMOOTH in rows)
            assertEquals(device.id, device.camera, DisplayRow.CAMERA in rows)
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

    @Test fun onlyTheZire72HasACameraRow() {
        assertEquals(listOf("palm-zire-72"), Device.entries.filter { DisplayRow.CAMERA in RunningSheets.displayRows(it) }.map { it.id })
        assertEquals(DisplayRow.CAMERA, RunningSheets.displayRows(Device.ZIRE72).last())
    }

    @Test fun audioRowsFollowTheHardware() {
        val vibrating = setOf("palm-m500", "palm-m505", "palm-m515", "palm-m520", "ibm-workpad-c500",
            "sony-clie-t415", "tapwave-zodiac-2")
        for (device in Device.entries) {
            val rows = RunningSheets.audioRows(device)
            assertEquals(AudioRow.SOUND, rows.first())
            assertEquals(device.id, !device.runs68k || device.microphone, AudioRow.MICROPHONE in rows)
            assertEquals(device.id, !device.runs68k, AudioRow.AUDIO_PREVIEW in rows)
            assertEquals(device.id, device.id in vibrating, AudioRow.RUMBLE in rows)
        }
    }

    @Test fun connectionRowsFollowTheDevice() {
        for (device in Device.entries) {
            val rows = RunningSheets.connectionRows(device, off, false)
            assertEquals(ConnRow.BEAM, rows.first())
            assertEquals(device.id, device.hotSyncButton, ConnRow.HOTSYNC in rows)
            assertEquals(device.id, device.zodiac, ConnRow.CRADLE in rows)
            assertEquals(device.id, !device.serialAbsent, ConnRow.SERIAL in rows)
            assertEquals(device.id, !device.zaurus && (!device.runs68k || device.usb), ConnRow.USB in rows)
            assertEquals(device.id, device.zaurus, ConnRow.ZAURUS_USB in rows)
            assertEquals(device.id, !device.runs68k || device.bluetooth, ConnRow.BLUETOOTH in rows)
            assertEquals(device.id, device.usbPrinter || device.usbWifi, ConnRow.USB_PORT in rows)
            assertEquals(device.id, device.builtInWifi, ConnRow.WIFI_MODULE in rows)
            assertFalse(ConnRow.DEBUGGER in rows)
            assertFalse(ConnRow.USB_RETRY in rows)
        }
    }

    @Test fun zaurusUsbShowsTheActualCableMode() {
        assertEquals(ZaurusUsbMode.OFF, ZaurusUsbMode.fromState("off Cable removed"))
        assertEquals(ZaurusUsbMode.PC_LINK, ZaurusUsbMode.fromState("pc-link running"))
        assertEquals(ZaurusUsbMode.DRIVE_PROBE, ZaurusUsbMode.fromState("storage-probe detached"))
        assertEquals(ZaurusUsbMode.ENUMERATION_PROBE, ZaurusUsbMode.fromState("probe running"))
        assertNull(ZaurusUsbMode.fromState("on running rndis up"))
        assertEquals(listOf("off", "pc-link", "storage-probe"), ZaurusUsbMode.choices(ZaurusUsbMode.OFF).map { it.first })
        assertTrue("probe" in ZaurusUsbMode.choices(ZaurusUsbMode.ENUMERATION_PROBE).map { it.first })
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
