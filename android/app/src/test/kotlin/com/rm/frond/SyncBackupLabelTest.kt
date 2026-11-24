package com.rm.frond

import com.rm.frond.ui.backupSourceLabel
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class SyncBackupLabelTest {
    private val name = { key: String -> if (key == "machine:a") "Palm m515" else key }

    @Test
    fun deviceBackupUsesTheDeviceName() {
        assertEquals("Palm m515", backupSourceLabel(JSONObject().put("device", "machine:a"), name))
    }

    @Test
    fun recoveryCopyShowsItsDeviceAndNotTheKey() {
        val row = JSONObject().put("device", "machine:a / recovery 0123abcd").put("owner", "machine:a").put("recovery", 1795000000L)
        val label = backupSourceLabel(row, name)
        assertEquals(true, label.startsWith("Palm m515 · Recovery copy 20"))
        assertFalse(label.contains("0123abcd"))
    }
}
