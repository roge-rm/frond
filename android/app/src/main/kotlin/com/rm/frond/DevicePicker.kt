package com.rm.frond

import android.content.Context

// The device picker's groups and Recent list. The group names and order come from deviceProfiles5.
object DevicePicker {
    // The groups with their devices, oldest release first (the inventory order breaks a tie).
    fun groups(devices: List<Device> = Device.shown): List<Pair<String, List<Device>>> =
        deviceGroups.map { name -> name to devices.filter { it.group == name }.sortedBy { it.released } }
            .filter { it.second.isNotEmpty() }

    const val RECENT_MAX = 3
    private const val FILE = "device-picker"
    private const val RECENT = "recent"

    // `id` goes to the front of the last 3 ids.
    fun pushRecent(recent: List<String>, id: String): List<String> =
        (listOf(id) + recent.filter { it != id }).take(RECENT_MAX)

    // The saved ids that are still devices, newest first.
    fun recent(context: Context, devices: List<Device> = Device.shown): List<Device> =
        parse(context.getSharedPreferences(FILE, Context.MODE_PRIVATE).getString(RECENT, null), devices)

    fun parse(saved: String?, devices: List<Device>): List<Device> =
        saved.orEmpty().split(',').mapNotNull { id -> devices.firstOrNull { it.id == id } }.take(RECENT_MAX)

    fun remember(context: Context, device: Device) {
        val p = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)
        val saved = p.getString(RECENT, null).orEmpty().split(',').filter { it.isNotEmpty() }
        p.edit().putString(RECENT, pushRecent(saved, device.id).joinToString(",")).apply()
    }
}
