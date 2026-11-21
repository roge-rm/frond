package com.rm.frond.ui

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.rm.frond.Session
import com.rm.frond.SyncFiles
import com.rm.frond.SyncKind
import com.rm.frond.SyncNative
import com.rm.frond.SyncProtocol
import com.rm.frond.supportsNetworkHotSync
import com.rm.frond.rows
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private const val DB_LIMIT = 256L * 1024 * 1024
private const val TEXT_LIMIT = 32L * 1024 * 1024
private const val DEVICE_FILE_LIMIT = 16L * 1024 * 1024

private class SyncWork(private val scope: CoroutineScope) {
    private val lock = Mutex()
    private var pending by mutableStateOf(0)
    val busy get() = pending != 0
    var error by mutableStateOf<String?>(null)
    var changed by mutableStateOf(0)
        private set

    fun reload() { changed++ }

    fun <T> run(change: Boolean = true, task: () -> T, then: (T) -> Unit = {}) {
        pending++
        scope.launch {
            try {
                lock.withLock {
                    then(withContext(Dispatchers.IO) { task() })
                    if (change) reload()
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = e.message ?: "Couldn't complete the sync manager operation."
            } finally {
                pending--
            }
        }
    }
}

@Composable
private fun SyncError(work: SyncWork) {
    work.error?.let { message ->
        AlertDialog(
            title = { Text("Sync Manager") }, text = { Text(message) },
            onDismissRequest = { work.error = null },
            confirmButton = { TextButton(onClick = { work.error = null }) { Text("OK") } },
        )
    }
}

private data class ProfileChoice(val id: Long, val name: String)
private fun JSONObject.profiles() = rows().map { ProfileChoice(it.getLong("id"), it.getString("name")) }

private val conduitActions = linkedMapOf(
    "sync" to "Synchronise",
    "desktop-overwrites" to "Profile overwrites device",
    "handheld-overwrites" to "Device overwrites profile",
    "skip" to "Skip",
)

@Composable
internal fun SyncDeviceDialog(
    machine: String,
    onClose: () -> Unit,
    start: (() -> Unit)? = null,
    protocol: SyncProtocol? = SyncProtocol.HOT_SYNC,
    networkHotSync: Boolean = false,
) {
    val context = LocalContext.current.applicationContext
    val key = SyncNative.deviceKey(machine)
    val palm = protocol == SyncProtocol.HOT_SYNC
    val winmo = protocol == SyncProtocol.ACTIVE_SYNC
    val deviceKind = if (palm) "palm" else "winmo"
    val scope = rememberCoroutineScope()
    val work = remember(scope) { SyncWork(scope) }
    var profiles by remember { mutableStateOf(emptyList<ProfileChoice>()) }
    var assigned by remember { mutableStateOf(0L) }
    var actions by remember { mutableStateOf(JSONObject()) }
    var newName by rememberSaveable { mutableStateOf("") }
    var creating by rememberSaveable { mutableStateOf(false) }
    var fileImportProfile by rememberSaveable { mutableStateOf(0L) }
    var fileExportProfile by rememberSaveable { mutableStateOf(0L) }
    var fileExportName by rememberSaveable { mutableStateOf<String?>(null) }
    val fileImporter = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val profile = fileImportProfile
        fileImportProfile = 0
        if (uri != null && profile != 0L) work.run(task = {
            val name = documentName(context, uri)
            withDocument(context, uri, DEVICE_FILE_LIMIT) { temp ->
                SyncNative.call(context, "activesync-file-add", "profile" to profile, "device" to key, "name" to name, "path" to temp.path)
            }
        })
    }
    val fileExporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val profile = fileExportProfile
        val name = fileExportName
        fileExportProfile = 0
        fileExportName = null
        if (uri != null && profile != 0L && name != null) work.run(change = false, task = {
            val temp = File.createTempFile("sync-file-", ".tmp", context.cacheDir)
            try {
                SyncNative.call(context, "activesync-file-export", "profile" to profile, "device" to key, "name" to name, "path" to temp.path)
                context.contentResolver.openOutputStream(uri)?.use { out -> temp.inputStream().use { it.copyTo(out) } }
                    ?: throw IOException("Couldn't save the profile file.")
            } finally { temp.delete() }
        })
    }

    LaunchedEffect(work.changed) {
        work.run(change = false, task = {
            val result = SyncNative.call(context, "profiles", "device" to key)
            val id = result.optLong("assigned")
            val operation = if (palm) "actions" else "activesync-settings"
            val values = if (id == 0L || protocol == null) JSONObject()
                else SyncNative.call(context, operation, "profile" to id, "device" to key)
            if (palm && networkHotSync && id != 0L) {
                val transport = SyncNative.call(context, "hotsync-transport", "profile" to id, "device" to key)
                values.put("connection", transport.getString("connection"))
            }
            result to values
        }) { (result, values) ->
            profiles = result.profiles()
            assigned = result.optLong("assigned")
            actions = values
        }
    }
    AlertDialog(
        onDismissRequest = { if (!work.busy) onClose() },
        title = { Text(if (start == null) "Sync settings" else "Choose a sync profile") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                if (work.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                if (profiles.isNotEmpty()) {
                    val choices = listOf(ProfileChoice(0, "None")) + profiles
                    Choice("Profile", choices.firstOrNull { it.id == assigned } ?: choices.first(), choices, { it.name }) { profile ->
                        if (work.busy) return@Choice
                        work.run(task = { SyncNative.call(context, "assign", "profile" to profile.id, "device" to key, "kind" to deviceKind) }) {
                            assigned = profile.id
                        }
                    }
                }
                if (creating) {
                    OutlinedTextField(newName, { newName = it }, label = { Text("Profile name") }, singleLine = true)
                    TextButton(enabled = newName.isNotBlank() && !work.busy, onClick = {
                        val name = newName.trim()
                        work.run(task = {
                            val id = SyncNative.call(context, "create", "name" to name).getLong("id")
                            SyncNative.call(context, "assign", "profile" to id, "device" to key, "kind" to deviceKind)
                        }) { creating = false; newName = "" }
                    }) { Text("Create and assign") }
                } else {
                    TextButton(enabled = !work.busy, onClick = { creating = true }) { Text("New profile") }
                }
                if ((winmo || networkHotSync) && assigned != 0L && start == null) {
                    val choices = if (winmo) mapOf("usb" to "USB", "serial" to "Serial")
                        else mapOf("local" to "USB or serial", "network" to "Network")
                    val connection = actions.optString("connection").takeIf(choices::containsKey) ?: choices.keys.first()
                    Choice("Connection", connection, choices.keys.toList(), { choices.getValue(it) }) {
                        if (work.busy) return@Choice
                        val id = assigned
                        work.run(task = {
                            val operation = if (winmo) "activesync-transport" else "hotsync-transport"
                            SyncNative.call(context, operation, "profile" to id, "device" to key, "value" to it)
                        })
                    }
                }
                if (winmo && assigned != 0L && start == null) {
                    val pairing = actions.optString("pairing")
                    Text(when (pairing) {
                        "confirmed" -> "Paired with profile"
                        "requested" -> "Pairing pending next sync"
                        else -> "No Frond partnership"
                    })
                    if (pairing != "confirmed") TextButton(enabled = !work.busy, onClick = {
                        val profile = assigned
                        work.run(task = {
                            SyncNative.call(context, "activesync-pair", "profile" to profile, "device" to key,
                                "request" to (pairing != "requested"))
                        })
                    }) { Text(if (pairing == "requested") "Cancel request" else "Pair on next sync") }
                    if (actions.optBoolean("filesEnabled")) {
                        TextButton(enabled = !work.busy, onClick = {
                            fileImportProfile = assigned
                            fileImporter.launch(arrayOf("*/*"))
                        }) { Text("Add profile file") }
                        val entries = actions.optJSONArray("files")
                        val files = if (entries == null) emptyList() else List(entries.length()) { entries.getString(it) }
                        if (!work.busy && files.isNotEmpty()) {
                            Choice("Save profile file", "", files, { it }) { name ->
                                fileExportProfile = assigned
                                fileExportName = name
                                fileExporter.launch(name)
                            }
                        }
                    } else {
                        TextButton(enabled = !work.busy, onClick = {
                            val profile = assigned
                            work.run(task = {
                                SyncNative.call(context, "activesync-files-enable", "profile" to profile, "device" to key)
                            })
                        }) { Text("Enable file exchange") }
                    }
                }
                if (palm && assigned != 0L && start == null) SyncKind.entries.forEach { kind ->
                    val action = actions.optString(kind.wire, "sync")
                    Choice(kind.label, action.takeIf(conduitActions::containsKey) ?: "sync", conduitActions.keys.toList(), { conduitActions.getValue(it) }) {
                        if (work.busy) return@Choice
                        val id = assigned
                        work.run(task = { SyncNative.call(context, "action", "profile" to id, "device" to key, "kind" to kind.wire, "value" to it) })
                    }
                }
            }
        },
        confirmButton = {
            TextButton(enabled = !work.busy && (start == null || assigned != 0L), onClick = { onClose(); start?.invoke() }) {
                Text(if (start == null) "Done" else "Start sync")
            }
        },
        dismissButton = if (start == null) null else { { TextButton(onClick = onClose, enabled = !work.busy) { Text("Cancel") } } },
    )
    SyncError(work)
}

private data class InstallerChoice(
    val uri: Uri,
    val name: String,
    val profile: Long,
    val device: String,
    val files: List<JSONObject>,
)

private enum class SyncSection(val label: String, val kind: SyncKind? = null) {
    CONTACTS("Contacts", SyncKind.CONTACTS), CALENDAR("Calendar", SyncKind.CALENDAR),
    TASKS("Tasks", SyncKind.TASKS), MEMOS("Memos", SyncKind.MEMOS),
    BACKUPS("Backups"), INSTALLS("Install queue"), LOG("Sync log")
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun SyncManagerScreen(machines: List<Session>, refresh: Int = 0, onClose: () -> Unit) {
    val context = LocalContext.current.applicationContext
    val scope = rememberCoroutineScope()
    val work = remember(scope) { SyncWork(scope) }
    var profiles by remember { mutableStateOf(emptyList<ProfileChoice>()) }
    var profileId by rememberSaveable { mutableStateOf(0L) }
    var section by rememberSaveable { mutableStateOf(SyncSection.CONTACTS) }
    var rows by remember { mutableStateOf(emptyList<JSONObject>()) }
    var assignments by remember { mutableStateOf(JSONObject()) }
    var assignment by rememberSaveable { mutableStateOf<String?>(null) }
    var profileName by rememberSaveable { mutableStateOf<String?>(null) }
    var renaming by rememberSaveable { mutableStateOf(false) }
    var editing by remember { mutableStateOf<JSONObject?>(null) }
    var confirm by remember { mutableStateOf<Pair<String, () -> Unit>?>(null) }
    var restoreFrom by rememberSaveable { mutableStateOf<String?>(null) }
    var installFor by rememberSaveable { mutableStateOf<String?>(null) }
    var exportRequest by rememberSaveable { mutableStateOf<String?>(null) }
    var importRequest by rememberSaveable { mutableStateOf<String?>(null) }
    var notice by remember { mutableStateOf<String?>(null) }
    var installerChoice by remember { mutableStateOf<InstallerChoice?>(null) }

    fun call(op: String, vararg args: Pair<String, Any>) = SyncNative.call(context, op, *args)

    LaunchedEffect(profileId, section, work.changed, refresh) {
        rows = emptyList()
        val selected = profileId
        val page = section
        work.run(change = false, task = {
            val library = call("profiles")
            val choices = library.profiles()
            val id = choices.firstOrNull { it.id == selected }?.id ?: choices.firstOrNull()?.id ?: 0L
            val values = if (id == 0L) emptyList() else when (page) {
                SyncSection.BACKUPS -> call("backups", "profile" to id).rows()
                SyncSection.INSTALLS -> call("queue", "profile" to id).rows()
                SyncSection.LOG -> call("log", "profile" to id).rows()
                else -> call("records", "profile" to id, "kind" to page.kind!!.wire).rows()
            }
            Triple(library, id, values)
        }) { (library, id, values) ->
            if (profileId == selected && section == page) {
                profiles = library.profiles()
                profileId = id
                rows = values
                assignments = library.getJSONObject("assignments")
            }
        }
    }

    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val arg = importRequest?.let(::JSONObject)
        importRequest = null
        if (uri != null && arg != null) work.run(task = {
            val profile = arg.getLong("profile")
            val name = documentName(context, uri)
            if (arg.optBoolean("install")) {
                val device = arg.getString("device")
                val installer = name.substringAfterLast('.').lowercase() in setOf("zip", "exe", "msi")
                withDocument(context, uri, DB_LIMIT) { temp ->
                    if (installer) {
                        val files = call("installer-files", "profile" to profile, "device" to device, "name" to name,
                            "path" to temp.path).rows()
                        InstallerChoice(uri, name, profile, device, files)
                    } else {
                        call("install", "profile" to profile, "device" to device, "name" to name, "path" to temp.path)
                        null
                    }
                }
            } else {
                val kind = SyncKind.valueOf(arg.getString("kind"))
                withDocument(context, uri, TEXT_LIMIT) { temp ->
                    val bytes = temp.readBytes()
                    val text = bytes.decodeToString(throwOnInvalidSequence = true).removePrefix("\uFEFF")
                    call("import", "profile" to profile, "kind" to kind.wire, "rows" to SyncFiles.read(name, kind, text))
                }
                null
            }
        }) { choice ->
            if (choice != null) installerChoice = choice
            else notice = if (arg.optBoolean("install")) "Install queued" else "Records imported"
        }
    }

    installerChoice?.let { choice ->
        AlertDialog(
            onDismissRequest = { installerChoice = null },
            title = { Text("Choose install files") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    choice.files.forEach { member ->
                        val name = member.getString("name")
                        TextButton(enabled = !work.busy, onClick = {
                            work.run(task = {
                                withDocument(context, choice.uri, DB_LIMIT) { temp ->
                                    call("install-member", "profile" to choice.profile, "device" to choice.device,
                                        "name" to choice.name, "path" to temp.path, "member" to name,
                                        "index" to member.getInt("index"))
                                }
                            }) { notice = "$name queued" }
                        }) { Text(name) }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { installerChoice = null }) { Text("Done") } },
        )
    }

    val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val arg = exportRequest?.let(::JSONObject)
        exportRequest = null
        if (uri != null && arg != null) work.run(change = false, task = {
            val profile = arg.getLong("profile")
            if (arg.has("backup")) {
                val source = arg.getJSONObject("backup")
                val temp = File.createTempFile("sync-backup-", ".tmp", context.cacheDir)
                try {
                    call("backup", "profile" to profile, "device" to source.getString("device"),
                        "key" to source.getString("key"), "path" to temp.path)
                    context.contentResolver.openOutputStream(uri)?.use { out -> temp.inputStream().use { it.copyTo(out) } }
                        ?: throw IOException("Couldn't write the backup.")
                } finally { temp.delete() }
            } else {
                val kind = SyncKind.valueOf(arg.getString("kind"))
                val records = call("records", "profile" to profile, "kind" to kind.wire).rows()
                    .filter { !arg.has("id") || it.getLong("id") == arg.getLong("id") }
                val data = SyncFiles.write(arg.getString("extension"), kind, records)
                context.contentResolver.openOutputStream(uri)?.use { it.write(data.toByteArray()) }
                    ?: throw IOException("Couldn't write the records.")
            }
        }) { notice = "File saved" }
    }

    val selected = profiles.firstOrNull { it.id == profileId }
    val syncMachines = machines.filter { it.device?.let { device -> SyncProtocol.forDevice(device) } != null }
    val assignedMachines = syncMachines.filter { assignments.optLong(SyncNative.deviceKey(it.id)) == profileId }
    val byKey = machines.associateBy { SyncNative.deviceKey(it.id) }
    fun machineName(key: String) = byKey[key]?.name ?: key.ifBlank { "Any device" }

    @Composable
    fun profileControls() {
        if (selected != null) {
            Choice("Profile", selected, profiles, { it.name }) { if (!work.busy) profileId = it.id }
            Row(horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                TextButton(enabled = !work.busy, onClick = { renaming = false; profileName = "" }) { Text("New profile") }
                TextButton(enabled = !work.busy, onClick = { renaming = true; profileName = selected.name }) { Text("Rename") }
                TextButton(enabled = !work.busy, onClick = {
                    confirm = "Delete ${selected.name} and its records, backups and queue?" to {
                        work.run(task = { call("delete", "profile" to selected.id) })
                    }
                }) { Text("Delete") }
            }
            if (syncMachines.isNotEmpty()) TextButton(enabled = !work.busy, onClick = { assignment = "" }) { Text("Device settings") }
        } else TextButton(enabled = !work.busy, onClick = { renaming = false; profileName = "" }) { Text("New profile") }
    }

    Scaffold(topBar = {
        TopAppBar(title = { Text("Sync Manager") }, navigationIcon = {
            TextButton(onClick = onClose) { Text("Back") }
        })
    }) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            item {
                Column(Modifier.padding(horizontal = 16.dp)) {
                    if (work.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                    notice?.let { Text(it, style = MaterialTheme.typography.labelMedium) }
                    profileControls()
                    if (selected != null) {
                        Choice("Show", section, SyncSection.entries, { it.label }) { if (!work.busy) { section = it; notice = null } }
                        val kind = section.kind
                        if (kind != null) {
                            Row {
                                TextButton(enabled = !work.busy, onClick = { editing = JSONObject() }) { Text("Add") }
                                TextButton(enabled = !work.busy, onClick = {
                                    importRequest = JSONObject().put("profile", selected.id).put("kind", kind.name).toString()
                                    importer.launch(arrayOf("*/*"))
                                }) { Text("Import") }
                                TextButton(enabled = !work.busy && rows.isNotEmpty(), onClick = {
                                    exportRequest = JSONObject().put("profile", selected.id).put("kind", kind.name)
                                        .put("extension", "csv").toString()
                                    exporter.launch("${kind.wire}.csv")
                                }) { Text("Export CSV") }
                                if (kind != SyncKind.MEMOS) TextButton(enabled = !work.busy && rows.isNotEmpty(), onClick = {
                                    val ext = SyncFiles.extension(kind)
                                    exportRequest = JSONObject().put("profile", selected.id).put("kind", kind.name).put("extension", ext).toString()
                                    exporter.launch("${kind.wire}.$ext")
                                }) { Text(if (kind == SyncKind.CONTACTS) "vCard" else "iCalendar") }
                            }
                        }
                        if (section == SyncSection.INSTALLS) TextButton(enabled = !work.busy, onClick = { installFor = "" }) { Text("Queue install") }
                    }
                }
            }
            if (selected != null) items(rows) { row ->
                when (section) {
                    SyncSection.BACKUPS -> ListItem(
                        headlineContent = { Text(row.optString("name")) },
                        supportingContent = { Text(machineName(row.optString("device"))) },
                        trailingContent = {
                            Row {
                                TextButton(enabled = !work.busy, onClick = {
                                    exportRequest = JSONObject().put("profile", selected.id).put("backup", row).toString()
                                    val name = row.getString("name").replace(Regex("[/\\\\]"), "_")
                                    exporter.launch("$name.${row.getString("extension")}")
                                }) { Text("Save") }
                                if (assignedMachines.isNotEmpty()) TextButton(enabled = !work.busy, onClick = { restoreFrom = row.getString("device") }) { Text("Restore") }
                            }
                        },
                    )
                    SyncSection.INSTALLS -> ListItem(
                        headlineContent = { Text(row.optString("name")) },
                        supportingContent = {
                            Text(listOf(machineName(row.optString("device")), row.optString("state"), row.optString("message"))
                                .filter(String::isNotBlank).joinToString(" · "))
                        },
                        trailingContent = { TextButton(enabled = !work.busy, onClick = {
                            work.run(task = { call("unqueue", "profile" to selected.id, "id" to row.getLong("id")) })
                        }) { Text("Remove") } },
                    )
                    SyncSection.LOG -> ListItem(
                        headlineContent = { Text(row.optString("text")) },
                        supportingContent = { Text("${machineName(row.optString("device"))} · ${readableTime(row.getLong("time"))}") },
                    )
                    else -> {
                        val kind = section.kind!!
                        ListItem(
                            headlineContent = { Text(recordTitle(kind, row), maxLines = 2) },
                            supportingContent = if (row.optString("category").isBlank()) null else { { Text(row.optString("category")) } },
                            modifier = Modifier.clickable(enabled = !work.busy) { editing = row },
                        )
                    }
                }
            }
        }
    }

    profileName?.let { name ->
        AlertDialog(
            title = { Text(if (renaming) "Rename profile" else "New profile") },
            onDismissRequest = { if (!work.busy) profileName = null },
            text = { OutlinedTextField(name, { profileName = it }, label = { Text("Name") }, singleLine = true) },
            confirmButton = {
                TextButton(enabled = name.isNotBlank() && !work.busy, onClick = {
                    val op = if (renaming) "rename" else "create"
                    work.run(task = { call(op, "profile" to profileId, "name" to name.trim()) }) {
                        if (op == "create") profileId = it.getLong("id")
                        profileName = null
                    }
                }) { Text("Save") }
            },
            dismissButton = { TextButton(onClick = { profileName = null }, enabled = !work.busy) { Text("Cancel") } },
        )
    }
    assignment?.let { key ->
        if (key.isEmpty()) MachinePicker("Sync settings", syncMachines, { assignment = null }) { assignment = it?.id }
        else SyncDeviceDialog(
            key,
            protocol = syncMachines.firstOrNull { it.id == key }?.device?.let { SyncProtocol.forDevice(it) },
            networkHotSync = syncMachines.firstOrNull { it.id == key }?.device?.supportsNetworkHotSync() == true,
            onClose = {
                assignment = null
                work.reload()
            },
        )
    }
    installFor?.let {
        MachinePicker("Queue install for", assignedMachines, { installFor = null }, any = true) { machine ->
            installFor = null
            importRequest = JSONObject().put("profile", profileId).put("install", true)
                .put("device", machine?.id?.let(SyncNative::deviceKey).orEmpty()).toString()
            importer.launch(arrayOf("*/*"))
        }
    }
    restoreFrom?.let { source ->
        MachinePicker("Restore backups to",
            assignedMachines.filter { it.device?.palmOs == true }, { restoreFrom = null }) { machine ->
            restoreFrom = null
            if (machine != null) confirm = "The next sync replaces databases on ${machine.name} with these backups." to {
                work.run(task = { call("restore", "profile" to profileId, "source" to source, "device" to SyncNative.deviceKey(machine.id)) }) {
                    notice = "${it.getInt("count")} databases queued"
                }
            }
        }
    }
    editing?.let { record ->
        val kind = section.kind
        if (kind != null && selected != null) SyncRecordEditor(kind, record, work.busy, {
            editing = null
        }, onSave = { updated ->
            work.run(task = { call("save", "profile" to selected.id, "kind" to kind.wire, "record" to updated) }) { editing = null }
        }, onDelete = {
            confirm = "Delete this record?" to {
                work.run(task = { call("remove", "profile" to selected.id, "kind" to kind.wire, "id" to record.getLong("id"),
                    "revision" to record.getLong("revision")) }) { editing = null }
            }
        }, export = {
            exportRequest = JSONObject().put("profile", selected.id).put("kind", kind.name)
                .put("extension", "txt").put("id", record.getLong("id")).toString()
            exporter.launch("memo.txt")
        })
    }
    confirm?.let { (message, action) ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            text = { Text(message) },
            confirmButton = { TextButton(enabled = !work.busy, onClick = { confirm = null; action() }) { Text("Continue") } },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel") } },
        )
    }
    SyncError(work)
}

@Composable
private fun MachinePicker(title: String, machines: List<Session>, close: () -> Unit, any: Boolean = false, pick: (Session?) -> Unit) {
    AlertDialog(
        onDismissRequest = close, title = { Text(title) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                if (any) TextButton(onClick = { pick(null) }) { Text("Any device in this profile") }
                machines.forEach { machine -> TextButton(onClick = { pick(machine) }) { Text(machine.name) } }
            }
        },
        confirmButton = { TextButton(onClick = close) { Text("Cancel") } },
    )
}

private fun recordTitle(kind: SyncKind, record: JSONObject): String {
    val fields = record.optJSONObject("fields") ?: JSONObject()
    return when (kind) {
        SyncKind.CONTACTS -> listOf(fields.optString("firstName"), fields.optString("lastName")).joinToString(" ").trim()
            .ifBlank { fields.optString("company") }
        SyncKind.CALENDAR, SyncKind.TASKS -> fields.optString("summary")
        SyncKind.MEMOS -> fields.optString("text").lineSequence().firstOrNull().orEmpty()
    }.ifBlank { "Untitled" }
}

private fun readableTime(seconds: Long) =
    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm").withZone(ZoneId.systemDefault()).format(Instant.ofEpochSecond(seconds))

private fun documentName(context: Context, uri: Uri): String =
    context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
        if (it.moveToFirst()) it.getString(0) else null
    } ?: uri.lastPathSegment ?: throw IOException("Couldn't read the file name.")

private fun <T> withDocument(context: Context, uri: Uri, limit: Long, action: (File) -> T): T {
    val temp = File.createTempFile("sync-import-", ".tmp", context.cacheDir)
    try {
        context.contentResolver.openInputStream(uri)?.use { input ->
            temp.outputStream().use { out ->
                val buffer = ByteArray(65536)
                var total = 0L
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    total += count
                    if (total > limit) throw IOException("The file is too large.")
                    out.write(buffer, 0, count)
                }
            }
        } ?: throw IOException("Couldn't read the file.")
        return action(temp)
    } finally { temp.delete() }
}

private fun recordFields(kind: SyncKind): List<Pair<String, String>> = when (kind) {
    SyncKind.CONTACTS -> buildList {
        addAll(listOf("firstName" to "First name", "lastName" to "Last name", "company" to "Company", "title" to "Job title"))
        for (i in 1..7) add("phone$i" to "Phone or email $i")
        for (i in 1..3) {
            val suffix = if (i == 1) "" else i.toString()
            val label = if (i == 1) "" else " ($i)"
            for ((key, name) in listOf("street" to "Street", "city" to "City", "state" to "Province or state",
                "postalCode" to "Postal code", "country" to "Country")) add("$key$suffix" to "$name$label")
        }
        add("birthday" to "Birthday (YYYY-MM-DD)")
        add("webpage" to "Website")
        for (i in 1..2) add("chat$i" to "Chat $i")
        for (i in 1..9) add("custom$i" to "Custom $i")
        add("note" to "Note")
    }
    SyncKind.CALENDAR -> listOf("summary" to "Summary", "date" to "Date (YYYY-MM-DD)", "startTime" to "Start (HH:MM)",
        "endTime" to "End (HH:MM)", "location" to "Location", "note" to "Note")
    SyncKind.TASKS -> listOf("summary" to "Summary", "dueDate" to "Due (YYYY-MM-DD)", "priority" to "Priority",
        "completionDate" to "Completed on (YYYY-MM-DD)", "note" to "Note")
    SyncKind.MEMOS -> listOf("text" to "Text")
}

@Composable
private fun SyncRecordEditor(
    kind: SyncKind, record: JSONObject, busy: Boolean, onClose: () -> Unit,
    onSave: (JSONObject) -> Unit, onDelete: () -> Unit, export: () -> Unit,
) {
    val known = remember(kind) { recordFields(kind) }
    val original = record.optJSONObject("fields") ?: JSONObject()
    val fields = remember(record) { mutableStateMapOf<String, String>().apply { known.forEach { (key, _) -> put(key, original.optString(key)) } } }
    val labels = listOf("Work", "Home", "Fax", "Other", "Email", "Main", "Pager", "Mobile")
    val phoneTypes = remember(record) { mutableStateMapOf<Int, Int>() }
    var category by remember(record) { mutableStateOf(record.optString("category")) }
    var secret by remember(record) { mutableStateOf(record.optBoolean("private")) }
    var archived by remember(record) { mutableStateOf(record.optBoolean("archived")) }
    var complete by remember(record) { mutableStateOf(original.optString("complete") == "1") }
    var error by remember(record) { mutableStateOf<String?>(null) }
    val existing = record.optLong("id") != 0L
    AlertDialog(
        onDismissRequest = { if (!busy) onClose() },
        title = { Text(if (existing) "Edit record" else "New record") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                OutlinedTextField(category, { category = it }, label = { Text("Category") }, singleLine = true)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(secret, { secret = it })
                    Text("Private")
                    Checkbox(archived, { archived = it })
                    Text("Archived")
                }
                if (kind == SyncKind.TASKS) Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(complete, { complete = it })
                    Text("Completed")
                }
                known.forEach { (key, label) ->
                    OutlinedTextField(fields.getValue(key), { fields[key] = it }, modifier = Modifier.fillMaxWidth(),
                        label = { Text(label) }, singleLine = key !in listOf("text", "note", "street", "street2", "street3"))
                    val phone = key.removePrefix("phone").toIntOrNull()
                    if (phone != null && fields.getValue(key).isNotEmpty()) {
                        val type = phoneTypes[phone] ?: original.optString("phoneLabel$phone").toIntOrNull()
                        Choice("Phone or email type", type?.takeIf { it in labels.indices } ?: 0,
                            labels.indices.toList(), { labels[it] }) { phoneTypes[phone] = it }
                    }
                }
                if (existing) Row {
                    TextButton(onClick = onDelete, enabled = !busy) { Text("Delete") }
                    if (kind == SyncKind.MEMOS) TextButton(onClick = export, enabled = !busy) { Text("Save text file") }
                }
            }
        },
        confirmButton = { TextButton(enabled = !busy, onClick = {
            error = validateFields(kind, fields)
            if (error == null) {
                val fresh = JSONObject(record.toString())
                val changed = JSONObject(original.toString())
                for ((key, _) in known) {
                    val value = fields.getValue(key)
                    if (value.isNotEmpty()) changed.put(key, value)
                    else if (original.optString(key).isNotEmpty()) changed.remove(key)
                }
                for ((phone, type) in phoneTypes) changed.put("phoneLabel$phone", type.toString())
                if (kind == SyncKind.TASKS) changed.put("complete", if (complete) "1" else "0")
                onSave(fresh.put("category", category.trim()).put("private", secret).put("archived", archived).put("fields", changed))
            }
        }) { Text("Save") } },
        dismissButton = { TextButton(onClick = onClose, enabled = !busy) { Text("Cancel") } },
    )
}

private fun validateFields(kind: SyncKind, fields: Map<String, String>): String? {
    val start = fields["startTime"].orEmpty()
    val end = fields["endTime"].orEmpty()
    if (kind == SyncKind.CALENDAR) {
        if (fields["date"].isNullOrEmpty()) return "Enter a date for the event."
        if (start.isEmpty() != end.isEmpty() || end < start) return "Enter both times in order, or leave both empty for an all-day event."
    }
    for (key in listOf("date", "birthday", "dueDate", "completionDate")) {
        val value = fields[key]?.takeIf(String::isNotEmpty) ?: continue
        val year = runCatching { LocalDate.parse(value).year }.getOrNull()
        if (year == null || year !in 1904..2031 || value.length != 10) return "Enter a date between 1904 and 2031 as YYYY-MM-DD."
    }
    for (value in listOf(start, end).filter(String::isNotEmpty)) {
        if (value.length != 5 || runCatching { LocalTime.parse(value) }.isFailure) return "Enter a time as HH:MM."
    }
    val priority = fields["priority"]?.takeIf(String::isNotEmpty)
    if (priority != null && (priority.toIntOrNull() ?: -1) !in 0..127) return "Enter a priority from 0 to 127."
    return null
}
