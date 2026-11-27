package com.rm.frond

import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

internal class ZaurusPcLink(
    private val host: ModelHost,
    private val link: ControlLink,
    private val pauseAudio: Boolean,
    private val soundEnabled: () -> Boolean,
    private val onClose: () -> Unit,
    private val notify: (String) -> Unit,
) {
    data class Entry(val name: String, val directory: Boolean)
    private data class Download(val file: File, val name: String, val picking: Boolean = false)

    var folder by mutableStateOf("")
        private set
    var entries by mutableStateOf<List<Entry>?>(null)
        private set
    var operation by mutableStateOf<String?>(null)
        private set
    var error by mutableStateOf<String?>(null)
        private set
    private var download by mutableStateOf<Download?>(null)
    val saveName get() = download?.takeUnless { it.picking }?.name
    val idle get() = operation == null && download == null && !closing

    private var task: Job? = null
    private var attached = false
    private var pausedSound: String? = null
    private var saving = false
    var closing by mutableStateOf(false)
        private set

    fun refresh() = openFolder(folder)

    fun openFolder(path: String) = work(if (attached) "Reading folder" else "Connecting to the Zaurus") {
        connect()
        list(path)
    }

    fun back() {
        if (folder.isEmpty() || operation != null) close()
        else openFolder(folder.substringBeforeLast('/', ""))
    }

    fun send(uri: Uri) = work("Sending file") {
        val name = withContext(Dispatchers.IO) { HostFiles.displayName(host.contentResolver, uri) }
        if (name == null || !validName(name)) throw IOException("This file needs a valid name.")
        withCacheFile { file ->
            withContext(Dispatchers.IO) {
                val input = host.contentResolver.openInputStream(uri) ?: throw IOException("Couldn't open the selected file.")
                input.use { source -> file.outputStream().use { copy(source, it) } }
            }
            connect()
            transfer("send", name, file)
        }
        notify("$name sent")
        list(folder)
    }

    fun receive(name: String) = work("Receiving file") {
        val file = cacheFile()
        try {
            connect()
            transfer("receive", name, file)
            download = Download(file, name)
        } finally {
            // The picker owns the staged file after a successful transfer.
            if (download?.file != file) file.delete()
        }
    }

    fun startSave(): String? {
        val picked = download?.takeUnless { it.picking } ?: return null
        download = picked.copy(picking = true)
        return picked.name
    }

    fun finishSave(uri: Uri?) {
        val picked = download?.takeIf { it.picking } ?: return
        download = null
        if (uri == null || closing) {
            picked.file.delete()
            return
        }
        saving = true
        work("Saving file") {
            try {
                withContext(Dispatchers.IO) {
                    val output = host.contentResolver.openOutputStream(uri, "wt")
                        ?: throw IOException("Couldn't open the destination file.")
                    output.use { target -> picked.file.inputStream().use { copy(it, target) } }
                }
                notify("${picked.name} received")
            } finally {
                picked.file.delete()
                saving = false
            }
        }
    }

    fun close() {
        if (closing) return
        closing = true
        val running = task
        val waitForSave = saving
        host.lifecycleScope.launch {
            if (waitForSave) running?.join() else running?.cancelAndJoin()
            disconnect()
            discardDownload()
            onClose()
        }
    }

    fun dispose() {
        task?.cancel()
        discardDownload()
    }

    private fun discardDownload() {
        download?.file?.delete()
        download = null
    }

    private fun work(label: String, body: suspend () -> Unit) {
        if (!idle) return
        operation = label
        error = null
        task = host.lifecycleScope.launch {
            try {
                body()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = e.message ?: "PC Link failed."
            } finally {
                operation = null
            }
        }
    }

    private suspend fun connect() {
        // Recheck the cable after a guest reset. The reply can race with cancellation.
        try {
            if (!attached && pauseAudio) holdSound()
            attached = true
            val reply = withContext(Dispatchers.IO) {
                link.request("zaurus-pc-link-open", timeoutSeconds = 10)
            }
            if (!reply.ok) {
                // A refused connection leaves other uses of the USB port alone.
                attached = reply.code == "timeout"
                throw IOException(reply.message)
            }
        } catch (e: Exception) {
            disconnect()
            throw e
        }
    }

    // The stock USB link can stall while Android Fast is playing audio.
    private suspend fun holdSound() = withContext(Dispatchers.IO) {
        val sound = link.request("sound-state", timeoutSeconds = 5)
        if (sound.ok && (sound.message == "on" || sound.message == "muted")) {
            pausedSound = sound.message
            val paused = link.request("audio-off", timeoutSeconds = 5)
            if (!paused.ok) throw IOException(paused.message)
        }
    }

    private suspend fun disconnect() {
        val sound = pausedSound
        if (!attached && sound == null) return
        withContext(NonCancellable + Dispatchers.IO) {
            if (attached) link.request("zaurus-pc-link-close", timeoutSeconds = 10)
            if (sound != null) {
                link.request("audio-on", timeoutSeconds = 5)
                if (sound == "muted" || !soundEnabled()) link.request("sound-mode", "off", timeoutSeconds = 5)
            }
        }
        attached = false
        pausedSound = null
    }

    private suspend fun list(path: String) {
        val result = JSONArray(runJob("list", path))
        val listed = List(result.length()) { n ->
            val item = result.getJSONObject(n)
            Entry(item.getString("name"), item.getBoolean("directory"))
        }
        folder = path
        entries = listed.filter { validName(it.name) }
            .sortedWith(compareByDescending<Entry> { it.directory }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name })
    }

    private suspend fun transfer(direction: String, name: String, file: File) {
        val remote = if (folder.isEmpty()) name else "$folder/$name"
        val argument = JSONObject().put("remote", remote).put("local", file.absolutePath)
        runJob(direction, argument.toString())
    }

    private suspend fun runJob(command: String, argument: String): String {
        var finished = false
        try {
            val started = withContext(Dispatchers.IO) {
                link.request("zaurus-pc-link-$command", argument, timeoutSeconds = 10)
            }
            if (!started.ok) {
                finished = started.code != "timeout"
                throw IOException(started.message)
            }
            if (started.code != "started") throw IOException("Couldn't start the PC Link request.")
            while (true) {
                delay(200)
                val reply = withContext(Dispatchers.IO) {
                    link.request("zaurus-pc-link-result", timeoutSeconds = 10)
                }
                if (reply.ok && reply.code == "busy") continue
                finished = reply.code != "timeout"
                if (!reply.ok) throw IOException(reply.message)
                if (reply.code != "done") throw IOException("Couldn't read the PC Link result.")
                return reply.message
            }
        } finally {
            if (!finished) disconnect()
        }
    }

    private fun cacheFile(): File = File.createTempFile("zaurus-pc-link-", ".tmp", host.cacheDir)

    private suspend fun withCacheFile(operation: suspend (File) -> Unit) {
        val file = cacheFile()
        try {
            operation(file)
        } finally {
            file.delete()
        }
    }

    private suspend fun copy(source: InputStream, target: OutputStream) {
        val buffer = ByteArray(64 * 1024)
        val context = currentCoroutineContext()
        while (true) {
            context.ensureActive()
            val count = source.read(buffer)
            if (count < 0) return
            target.write(buffer, 0, count)
        }
    }

    private fun validName(name: String) =
        name.isNotBlank() && name != "." && name != ".." && name.none { it == '/' || it == '\\' || it == '\u0000' }
}
