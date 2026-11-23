package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BatteryTest {
    private val host = HostBattery(42, true)

    @Test fun specText() {
        assertEquals("max battery", BatteryText.spec(BatteryMode.MAX, 100, false, host))
        assertEquals("max charging", BatteryText.spec(BatteryMode.MAX, 100, true, host))
        assertEquals("level 30 battery", BatteryText.spec(BatteryMode.LEVEL, 30, false, host))
        assertEquals("level 100 charging", BatteryText.spec(BatteryMode.LEVEL, 250, true, host))
        assertEquals("level 0 battery", BatteryText.spec(BatteryMode.LEVEL, -5, false, host))
        assertEquals("host 42 charging", BatteryText.spec(BatteryMode.HOST, 7, false, host))
        assertEquals("host 42 battery", BatteryText.spec(BatteryMode.HOST, 7, true, host.copy(charging = false)))
    }

    @Test fun machineThatCantChargeSaysBattery() {
        assertEquals("max battery", BatteryText.spec(BatteryMode.MAX, 100, true, host, chargingSupported = false))
        assertEquals("level 20 battery", BatteryText.spec(BatteryMode.LEVEL, 20, true, host, chargingSupported = false))
        assertEquals("host 42 battery", BatteryText.spec(BatteryMode.HOST, 0, false, host, chargingSupported = false))
    }

    @Test fun intentToState() {
        // plugged: 0 battery, 1 AC, 2 USB; status 2 charging, 3 discharging, 4 not charging, 5 full.
        assertEquals(HostBattery(50, false), BatteryText.host(50, 100, 0, 3))
        assertEquals(HostBattery(80, true), BatteryText.host(160, 200, 1, 2))
        assertEquals(HostBattery(33, false), BatteryText.host(33, 100, 0, 2))
        // Full a while after unplugging at 100%: not charging.
        assertEquals(HostBattery(100, false), BatteryText.host(100, 100, 0, 5))
        assertEquals(HostBattery(100, true), BatteryText.host(100, 100, 1, 5))
        assertEquals(HostBattery(7, true), BatteryText.host(7, 100, 2, 4))
        assertEquals(HostBattery(67, false), BatteryText.host(2, 3, 0, 3))
    }

    @Test fun oddIntentsReadFull() {
        assertEquals(HostBattery(100, false), BatteryText.host(-1, 100, 0, 3))
        assertEquals(HostBattery(100, false), BatteryText.host(50, 0, 0, 3))
        assertEquals(HostBattery(100, true), BatteryText.host(20, 100, 0, 3, present = false))
        assertEquals(100, BatteryText.host(120, 100, 0, 3).percent)
    }

    @Test fun coreReply() {
        assertEquals(BatteryInfo(true, false, true), BatteryInfo.parse("unavailable"))
        assertEquals(BatteryInfo(true, true, true), BatteryInfo.parse("max 100 battery"))
        assertEquals(BatteryInfo(true, true, false), BatteryInfo.parse("level 40 battery no-charging"))
        assertFalse(BatteryInfo().known)
    }

    @Test fun savedOnlyWhenNotDefault() {
        val json = MachineSettings().json()
        assertFalse(json.has("batteryMode"))
        assertFalse(json.has("batteryLevel"))
        assertFalse(json.has("batteryCharging"))
        val set = MachineSettings(batteryMode = BatteryMode.LEVEL, batteryLevel = 15, batteryCharging = false)
        val back = MachineSettings.from(set.json())
        assertEquals(BatteryMode.LEVEL, back.batteryMode)
        assertEquals(15, back.batteryLevel)
        assertFalse(back.batteryCharging)
        assertEquals(BatteryMode.HOST, MachineSettings.from(MachineSettings(batteryMode = BatteryMode.HOST).json()).batteryMode)
    }

    @Test fun oldSettingsAreMaximumOnTheCharger() {
        val old = MachineSettings.from(JSONObject().put("engine", "fast"))
        assertEquals(BatteryMode.MAX, old.batteryMode)
        assertEquals(100, old.batteryLevel)
        assertTrue(old.batteryCharging)
        assertEquals("max charging", BatteryText.spec(old.batteryMode, old.batteryLevel, old.batteryCharging, HostBattery()))
        assertFalse(MachineSettings.from(JSONObject().put("batteryCharging", false)).batteryCharging)
        assertEquals(100, MachineSettings.from(JSONObject().put("batteryLevel", 900)).batteryLevel)
        assertEquals(BatteryMode.MAX, MachineSettings.from(JSONObject().put("batteryMode", "nonsense")).batteryMode)
    }
}
