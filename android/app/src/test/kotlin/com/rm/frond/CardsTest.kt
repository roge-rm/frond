package com.rm.frond

import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.util.zip.CRC32
import java.util.zip.Deflater

// The card library (Cards.kt) and the machine operations that keep cards: move-in, delete,
// duplicate, export and import, cut-off moves, crash leftovers, serial collisions and linked
// machines taking turns. Card files are tiny (512-byte SD cards); a stand-in card tool makes them.
class CardsTest {
    @get:Rule val tmp = TemporaryFolder()
    private lateinit var data: File
    private lateinit var sessions: File
    private lateinit var library: File
    private val toolCalls = mutableListOf<List<String>>()

    @Before fun setUp() {
        data = tmp.newFolder("files")
        sessions = File(data, "sessions").apply { mkdirs() }
        library = Cards.libraryFor(sessions)
        Cards.freeSpace = { Long.MAX_VALUE / 4 }
        Cards.tool = ::fakeTool
    }

    @After fun tearDown() {
        Cards.freeSpace = { it.usableSpace }
        Cards.tool = { args, listener, cancel -> CardTool.run(args, listener, cancel) }
    }

    // ---- Files ----

    private fun put32(v: Long) = ByteArray(4) { (v ushr (8 * it)).toByte() }
    private fun put64(v: Long) = ByteArray(8) { (v ushr (8 * it)).toByte() }

    class Media(val kind: Long = 1, val logical: Long = 512, val physical: Long = 512, val generation: Long = 1,
                val identity: String = "c4c4c4c4frondcardstoretest0251c4", val writeProtected: Boolean = false)

    // A v8 .frond header with media descriptors, enough for the store.
    private fun frond(device: String, media: List<Media>): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(put32(0x19800819) + put32(8) + device.toByteArray(Charsets.US_ASCII).copyOf(32))
        out.write(put32(0) + put32(0) + ByteArray(40) + put32(media.size.toLong()))
        for (m in media) {
            val name = Cards.cardFiles[(m.kind - 1).toInt()]
            out.write(put32(m.kind) + name.toByteArray(Charsets.US_ASCII).copyOf(32) + put32(1) + put32(if (m.writeProtected) 1 else 0) +
                m.identity.toByteArray(Charsets.US_ASCII).copyOf(32) + put64(m.logical) + put64(m.physical) + put64(m.generation))
        }
        out.write("body".toByteArray())
        return out.toByteArray()
    }

    // A v2 .frondsd with a 512-byte payload (or the one given) at `generation`.
    private fun sdCard(generation: Long, payload: ByteArray = ByteArray(512) { 0xa5.toByte() }): ByteArray {
        val chunks = (payload.size + (1 shl 20) - 1) / (1 shl 20)
        val table = ByteArrayOutputStream().also { t ->
            for (c in 0 until chunks) {
                val crc = CRC32()
                crc.update(payload, c * (1 shl 20), minOf(1 shl 20, payload.size - c * (1 shl 20)))
                t.write(put32(crc.value))
            }
        }.toByteArray()
        val header = "FRONDSD".toByteArray(Charsets.US_ASCII) + byteArrayOf(0) + put32(2) + put32(512) +
            put64(payload.size.toLong()) + put32(0) + put64(generation) + put64(64L + payload.size) + put32(1L shl 20) + ByteArray(16)
        val crc = CRC32().also { it.update(header); it.update(table) }.value
        return header.copyOfRange(0, 24) + put32(crc) + header.copyOfRange(28, 64) + payload + table
    }

    private fun payloadOf(card: File): ByteArray {
        val bytes = card.readBytes()
        val size = le64(bytes, 16).toInt()
        return bytes.copyOfRange(64, 64 + size)
    }

    // A classic stick of `mb` MB with change counter `counter`.
    private fun stick(mb: Int, counter: Long): ByteArray {
        val (blocks, pages) = mapOf(4 to (512L to 16L), 8 to (1024L to 16L), 16 to (1024L to 32L))[mb]!!
        return "FRONDMS".toByteArray(Charsets.US_ASCII) + byteArrayOf(0) + put32(1) + put32(blocks) + put32(pages) +
            put32(512) + put32(9) + ByteArray(4) + put64(counter) + ByteArray(24) + ByteArray(Device.memoryStickPhysicalBytes(mb).toInt())
    }

    // A saved machine: files/sessions/<id>/ with <id>.json, session.frond and, with a card, sd.frondsd.
    private fun machine(name: String, card: Boolean, device: String = "palm-tx", identity: String = name.padEnd(32, '#').take(32),
                        generation: Long = 1): File {
        val dir = File(sessions, name.lowercase().replace(' ', '-')).apply { mkdirs() }
        File(dir, "${dir.name}.json").writeText("""{"name":"$name","device":"$device","created":1,"used":0}""")
        File(dir, "session.frond").writeBytes(frond(device, if (card) listOf(Media(identity = identity, generation = generation)) else emptyList()))
        if (card) File(dir, "sd.frondsd").writeBytes(sdCard(generation, ByteArray(512) { (name.hashCode() + it).toByte() }))
        return dir
    }

    // A drawer card made directly.
    private fun drawerCard(name: String, kind: String = "sd"): Cards.Card {
        val card = Cards.Card(Cards.newId(), name, kind, Cards.now(), null, 32L shl 20, Cards.newIdentity(), false)
        val dir = Cards.entryDir(library, card.id).apply { mkdirs() }
        assertTrue(Cards.writeCard(dir, card))
        File(dir, Cards.fileOfKind(kind)).writeBytes(sdCard(1))
        return card
    }

    // Card sizes come from the descriptors, 512 bytes here; links need a size a device takes.
    private fun grow(name: String) {
        val c = named(name)!!
        assertTrue(Cards.writeCard(Cards.entryDir(library, c.id), c.copy(bytes = 32L shl 20)))
    }
    private fun cards() = Cards.entries(library).map { it.card }
    private fun named(name: String) = cards().firstOrNull { it.name == name }
    private fun inDrawer(card: Cards.Card) = File(Cards.entryDir(library, card.id), Cards.fileOfKind(card.kind)).isFile

    // The card tool: make writes an empty card, import wraps a raw image, export unwraps.
    private fun fakeTool(args: Array<String>, listener: CardToolProgress?, cancel: java.util.concurrent.atomic.AtomicBoolean?): String {
        toolCalls += args.toList()
        listener?.progress(1, 2)
        if (cancel?.get() == true) return "error cancelled Cancelled."
        return when (args[0]) {
            "make" -> { File(args[3]).writeBytes(sdCard(1, ByteArray(512))); "ok ${args[1]} 512" }
            "import" -> { val raw = File(args[1]).readBytes(); File(args[3]).writeBytes(sdCard(1, raw)); "ok ${args[2]} ${raw.size}" }
            "export" -> { File(args[2]).writeBytes(payloadOf(File(args[1]))); "ok image 512" }
            // The fake "fd" is a path, so the test can read the result.
            "export-fd" -> { File(args[2]).writeBytes(payloadOf(File(args[1]))); "ok image 512" }
            else -> "error usage bad"
        }
    }

    // Holds a machine's run lock, as the :emulator process does while it runs.
    private fun running(dir: File) = RandomAccessFile(File(dir, Cards.RUN_LOCK), "rw").let { f -> f to f.channel.lock() }

    // ---- JSON ----

    @Test fun cardAndSlotJsonMatchTheDesktop() {
        val dir = File(library, "2b7c2a52-0000-4000-8000-000000000001").apply { mkdirs() }
        // As the desktop writes it.
        File(dir, "card.json").writeText("""{"bytes":33554432,"created":"2026-11-06T10:00:00Z","id":"${dir.name}","identity":"${"61".repeat(32)}","kind":"sd","machine":null,"name":"Games","version":1,"writeProtected":false}""")
        val card = Cards.readCard(dir)!!
        assertEquals("Games", card.name)
        assertNull(card.machine)
        assertEquals(33554432L, card.bytes)
        assertArrayEquals(ByteArray(32) { 'a'.code.toByte() }, card.identity)
        assertTrue(Cards.writeCard(dir, card.copy(machine = "x")))
        assertEquals(card.copy(machine = "x"), Cards.readCard(dir))
        val m = File(sessions, "m").apply { mkdirs() }
        File(m, "slot.json").writeText("""{"card":null,"link":"abc","name":null,"swap":true,"version":1}""")
        assertEquals(Cards.Slot(true, null, "abc", null, true), Cards.readSlot(m))
        assertTrue(Cards.writeSlot(m, Cards.Slot(true, "c", null, "Games", false)))
        assertEquals(Cards.Slot(true, "c", null, "Games", false), Cards.readSlot(m))
        assertTrue(org.json.JSONObject(File(m, "slot.json").readText()).isNull("link"))
        assertEquals(32, Cards.newIdentity().size)
        assertTrue(packableName("slot.json"))
    }

    // ---- Move-in ----

    @Test fun moveIn() {
        val a = machine("Palm TX", true)
        machine("Empty", false)
        // A v6 machine is left alone.
        val old = File(sessions, "old").apply { mkdirs() }
        File(old, "old.json").writeText("""{"name":"Old","device":"palm-tx"}""")
        File(old, "session.frond").writeBytes(put32(0x19800819) + put32(6) + ByteArray(64))
        Cards.startUp(sessions)
        val card = cards().single()
        assertEquals("Palm TX card", card.name)
        assertEquals("sd", card.kind)
        assertEquals(512L, card.bytes)
        assertArrayEquals("Palm TX".padEnd(32, '#').toByteArray(), card.identity)
        assertEquals(a.name, card.machine)
        assertEquals(Cards.Slot(true, card.id, null, "Palm TX card", false), Cards.readSlot(a))
        assertTrue(File(a, "sd.frondsd").isFile)
        assertFalse(Cards.readSlot(File(sessions, "empty")).present)
        // Again: nothing changes.
        Cards.reconcile(sessions)
        assertEquals(1, cards().size)
        // A folder copied by hand holds the same id: the one the hint doesn't name gets "Palm TX card 2",
        // with its own serial.
        val copy = File(sessions, "copy").apply { mkdirs() }
        File(copy, "copy.json").writeText("""{"name":"Copy","device":"palm-tx"}""")
        for (f in listOf("session.frond", "sd.frondsd", "slot.json")) File(a, f).copyTo(File(copy, f))
        Cards.reconcile(sessions)
        assertEquals(card.id, Cards.readSlot(a).card)
        val second = named("Palm TX card 2")!!
        assertEquals("copy", second.machine)
        assertFalse(second.identity.contentEquals(card.identity))
        assertEquals(second.id, Cards.readSlot(copy).card)
        assertTrue(Cards.readSlot(copy).swap)
        // A machine running in the emulator is skipped, then done.
        val busy = machine("Busy", true)
        val (file, lock) = running(busy)
        Cards.reconcile(sessions)
        assertFalse(Cards.readSlot(busy).present)
        lock.release(); file.close()
        Cards.reconcile(sessions)
        assertEquals(named("Busy card")!!.id, Cards.readSlot(busy).card)
        // Two machines with one serial and no slot.json: the second gets a new serial and loads it as a swap.
        val twinA = machine("Twin A", true, identity = "twin".padEnd(32, '#'))
        val twinB = machine("Twin B", true, identity = "twin".padEnd(32, '#'))
        Cards.reconcile(sessions)
        assertArrayEquals("twin".padEnd(32, '#').toByteArray(), named("Twin A card")!!.identity)
        assertFalse(Cards.readSlot(twinA).swap)
        val twin = named("Twin B card")!!
        assertFalse(twin.identity.contentEquals("twin".padEnd(32, '#').toByteArray()))
        assertTrue(Cards.readSlot(twinB).swap)
        assertEquals(listOf("--card-swap", "--card-identity", Cards.hex(twin.identity)), Cards.launchArguments(sessions, twinB))
    }

    // ---- Delete ----

    @Test fun deleteKeepsTheCard() {
        // Not moved in yet: delete moves it in first.
        val a = machine("Palm TX", true)
        val before = File(a, "sd.frondsd").readBytes()
        assertNull(deleteMachine(sessions, a, "Palm TX", true))
        assertFalse(a.exists())
        var card = cards().single()
        assertEquals("Palm TX card", card.name)
        assertNull(card.machine)
        assertArrayEquals(before, File(Cards.entryDir(library, card.id), "sd.frondsd").readBytes())
        // Unsettled: the save's full copy at generation 2 is what the .frond describes.
        val b = machine("Tungsten", true)
        File(b, "session.frond").writeBytes(frond("palm-tx", listOf(Media(generation = 2, identity = "t".repeat(32)))))
        File(b, "sd.frondsd.journal-2.full").writeBytes(sdCard(2))
        assertNull(deleteMachine(sessions, b, "Tungsten", true))
        assertFalse(b.exists())
        card = named("Tungsten card")!!
        assertEquals(2L, Cards.cardHeader(File(Cards.entryDir(library, card.id), "sd.frondsd"))!!.first)
        // A journal that can't be applied: refused, nothing removed.
        val c = machine("Zire", true)
        File(c, "session.frond").writeBytes(frond("palm-tx", listOf(Media(generation = 2, identity = "z".repeat(32)))))
        File(c, "sd.frondsd.journal-2").writeText("broken")
        assertEquals("Open Zire once to finish its last save.", deleteMachine(sessions, c, "Zire", true))
        assertTrue(File(c, "sd.frondsd").isFile && File(c, "session.frond").isFile)
        // A full disk stops the settle copy the same way.
        File(c, "sd.frondsd.journal-2").delete()
        File(c, "sd.frondsd").copyTo(File(c, "sd.frondsd.journal-9"))
        File(c, "session.frond").writeBytes(frond("palm-tx", listOf(Media(generation = 3, identity = "z".repeat(32)))))
        File(c, "sd.frondsd.journal-9").delete()
        File(c, "sd.frondsd.journal-3.full").writeBytes(sdCard(3))
        File(c, "sd.frondsd.journal-3").writeText("pending")
        Cards.freeSpace = { 1L shl 20 }
        assertTrue(deleteMachine(sessions, c, "Zire", true)!!.startsWith("Not enough free space"))
        Cards.freeSpace = { Long.MAX_VALUE / 4 }
        assertTrue(File(c, "session.frond").isFile && File(c, "sd.frondsd").isFile)
        assertTrue(library.list()!!.none { it.startsWith(".new-") })
        // Running in the emulator: refused.
        val d = machine("Open", true)
        val (file, lock) = running(d)
        assertEquals("Stop Open first.", deleteMachine(sessions, d, "Open", true))
        assertTrue(File(d, "sd.frondsd").isFile)
        lock.release(); file.close()
        // A linked card keeps its other links and becomes free.
        val e = machine("Linked", true)
        val f = machine("Partner", false)
        Cards.reconcile(sessions)
        grow("Linked card")
        val linked = named("Linked card")!!
        assertNull(Cards.setLinks(sessions, linked.id, listOf(e, f)))
        assertNull(deleteMachine(sessions, e, "Linked", true))
        assertTrue(inDrawer(named("Linked card")!!))
        assertEquals(linked.id, Cards.readSlot(f).link)
    }

    // ---- Duplicate ----

    @Test fun duplicateThreeWays() {
        val a = machine("Palm TX", true)
        Cards.reconcile(sessions)
        val original = named("Palm TX card")!!
        // Copy: a settled copy as a new card in the copy, with its own serial, loaded as a swap.
        val copyDir = File(sessions, "copy")
        assertNull(dup(a, copyDir, CardChoice.COPY))
        val copy = named("Palm TX card 2")!!
        assertFalse(copy.identity.contentEquals(original.identity))
        assertEquals("copy", copy.machine)
        assertEquals(Cards.Slot(true, copy.id, null, "Palm TX card 2", true), Cards.readSlot(copyDir))
        assertArrayEquals(File(a, "sd.frondsd").readBytes(), File(copyDir, "sd.frondsd").readBytes())
        assertEquals(listOf("--card-swap", "--card-identity", Cards.hex(copy.identity)), Cards.launchArguments(sessions, copyDir))
        // Link: nothing copied, both linked.
        val linkDir = File(sessions, "link")
        assertNull(dup(a, linkDir, CardChoice.LINK))
        assertEquals(Cards.Slot(true, null, original.id, null, true), Cards.readSlot(linkDir))
        assertNull(Cards.cardFileIn(linkDir))
        assertEquals(original.id, Cards.readSlot(a).link)
        assertEquals(original.id, Cards.readSlot(a).card)
        // No card.
        val noneDir = File(sessions, "none")
        assertNull(dup(a, noneDir, CardChoice.NONE))
        assertEquals(Cards.Slot(true, null, null, null, true), Cards.readSlot(noneDir))
        assertNull(Cards.cardFileIn(noneDir))
        assertEquals(2, cards().size)
        // A linked card held elsewhere: Copy is refused, Link works.
        File(linkDir, "linkdir.json").writeText("""{"name":"Link","device":"palm-tx"}""")
        File(linkDir, "${linkDir.name}.json").writeText("""{"name":"Link","device":"palm-tx"}""")
        assertEquals("Its card isn't in it. Link it or leave it out.", duplicateMachine(sessions, linkDir, File(sessions, "x"), { false }, CardChoice.COPY))
        assertFalse(File(sessions, "x").exists())
        assertNull(dup(linkDir, File(sessions, "y"), CardChoice.LINK))
        assertEquals(original.id, Cards.readSlot(File(sessions, "y")).link)
        // The linked original was stopped holding its card, so reconcile put it back in the drawer.
        assertTrue(inDrawer(original))
        // Cancelled and full disk: nothing left.
        val second = machine("Second", true)
        Cards.reconcile(sessions)
        assertEquals(COPY_CANCELLED, duplicateMachine(sessions, second, File(sessions, "z"), { true }, CardChoice.COPY))
        Cards.freeSpace = { 0L }
        assertTrue(duplicateMachine(sessions, second, File(sessions, "z"), { false }, CardChoice.COPY)!!.startsWith("Not enough free space"))
        assertFalse(File(sessions, "z").exists())
        assertEquals(3, cards().size)
    }

    // As Sessions.duplicate: the copy's marker goes once it's listed.
    private fun dup(from: File, dest: File, choice: CardChoice): String? =
        duplicateMachine(sessions, from, dest, { false }, choice).also { if (it == null) File(dest, COPY_PART).delete() }

    // ---- Export and import ----

    // Export then import, as Sessions.export and Sessions.import do.
    private fun exportImport(from: File, name: String): Pair<File, String?> {
        val zip = ByteArrayOutputStream().also { zipPackable(from, it) }.toByteArray()
        val dest = File(sessions, name.lowercase().replace(' ', '-')).apply { mkdirs() }
        val staged = File(dest, ".import.part")
        assertTrue(unzipPackable(zip.inputStream(), staged))
        val frond = sessionFile(staged)!!
        checkImportedSiblings(staged, frond, Cards.readSlot(staged).swap)?.let { return dest to it }
        assertTrue(frond.renameTo(File(dest, "session.frond")))
        for (f in listOf("sd.frondsd", "ms.frondms", "ms.frondmsp", "slot.json")) File(staged, f).takeIf { it.isFile }?.renameTo(File(dest, f))
        staged.deleteRecursively()
        val error = Cards.adoptImportedCard(sessions, dest, "$name card")
        File(dest, "${dest.name}.json").writeText("""{"name":"$name","device":"palm-tx"}""")
        return dest to error
    }

    @Test fun exportImportRoundTrip() {
        val a = machine("Palm TX", true)
        val partner = machine("Partner", false)
        Cards.reconcile(sessions)
        grow("Palm TX card")
        val card = named("Palm TX card")!!
        assertNull(Cards.setLinks(sessions, card.id, listOf(a, partner)))
        val (b, error) = exportImport(a, "Trip")
        assertNull(error)
        val imported = named("Palm TX card 2")!!
        assertFalse(imported.identity.contentEquals(card.identity))
        val slot = Cards.readSlot(b)
        assertEquals(imported.id, slot.card)
        assertNull(slot.link)
        assertTrue(slot.swap)
        assertArrayEquals(File(a, "sd.frondsd").readBytes(), File(b, "sd.frondsd").readBytes())
        assertEquals(card.id, Cards.readSlot(a).card)
        // Exported again: another new card.
        assertNull(exportImport(b, "Trip Two").second)
        assertEquals(3, cards().size)
        // An export from an older Frond has no slot.json: its card moves in the same way.
        val old = machine("Old", true)
        File(old, "slot.json").delete()
        val (c, oldError) = exportImport(old, "Older")
        assertNull(oldError)
        assertEquals(named("Older card")!!.id, Cards.readSlot(c).card)
        // A linked machine whose card is elsewhere: no card, no link.
        assertNull(Cards.reconcile(sessions))
        assertNull(Cards.releaseAtClose(sessions, a))
        assertTrue(inDrawer(named("Palm TX card")!!))
        val (d, linkedError) = exportImport(a, "Without")
        assertNull(linkedError)
        assertEquals(Cards.Slot(true, null, null, "Palm TX card", true), Cards.readSlot(d))
    }

    @Test fun importChecksClassicSticks() {
        // A 4 MB stick written by a machine (counter 3) imports; a wrong counter doesn't.
        val dir = tmp.newFolder("stick")
        val frond = File(dir, "session.frond")
        frond.writeBytes(frond("sony-clie-tg50", listOf(Media(2, 4L shl 20, Device.memoryStickPhysicalBytes(4), 3))))
        File(dir, "ms.frondms").writeBytes(stick(4, 3))
        assertNull(checkImportedSiblings(dir, frond))
        File(dir, "ms.frondms").writeBytes(stick(4, 2))
        assertEquals("That session's card files don't match.", checkImportedSiblings(dir, frond))
        // With swap, the stick is checked against its own header.
        assertNull(checkImportedSiblings(dir, frond, swap = true))
        File(dir, "ms.frondms").writeBytes(stick(8, 0))
        frond.writeBytes(frond("sony-clie-tg50", listOf(Media(2, 8L shl 20, Device.memoryStickPhysicalBytes(8), 0))))
        assertNull(checkImportedSiblings(dir, frond))
        // A swapped SD machine whose card is now another one.
        val sd = tmp.newFolder("sd")
        File(sd, "session.frond").writeBytes(frond("palm-tx", listOf(Media(generation = 4))))
        File(sd, "sd.frondsd").writeBytes(sdCard(9))
        assertEquals("That session's card files don't match.", checkImportedSiblings(sd, File(sd, "session.frond")))
        assertNull(checkImportedSiblings(sd, File(sd, "session.frond"), swap = true))
        File(sd, "sd.frondsd").delete()
        assertNull(checkImportedSiblings(sd, File(sd, "session.frond"), swap = true))
    }

    // ---- Cut-off moves and crashes ----

    @Test fun cutOffMoves() {
        val a = machine("Palm TX", true)
        Cards.reconcile(sessions)
        val card = named("Palm TX card")!!
        // Rule 4: the rename into the drawer went through, the slot wasn't updated.
        assertTrue(File(a, "sd.frondsd").renameTo(File(Cards.entryDir(library, card.id), "sd.frondsd")))
        Cards.reconcile(sessions)
        assertEquals(Cards.Slot(true, null, null, "Palm TX card", true), Cards.readSlot(a))
        assertNull(named("Palm TX card")!!.machine)
        // A put-in cut off before the rename: the drawer wins.
        Cards.writeSlot(a, Cards.readSlot(a).copy(card = card.id))
        Cards.reconcile(sessions)
        assertNull(Cards.readSlot(a).card)
        assertTrue(inDrawer(card))
        // Rule 5: an entry with no file anywhere goes, with links to it.
        val b = machine("Other", false)
        grow("Palm TX card")
        assertNull(Cards.setLinks(sessions, card.id, listOf(b)))
        assertEquals(card.id, Cards.readSlot(b).link)
        File(Cards.entryDir(library, card.id), "sd.frondsd").delete()
        Cards.reconcile(sessions)
        assertNull(named("Palm TX card"))
        assertNull(Cards.readSlot(b).link)
    }

    @Test fun crashLeftovers() {
        val now = System.currentTimeMillis()
        library.mkdirs()
        val stale = File(library, ".new-stale").apply { mkdirs(); File(this, "sd.frondsd").writeText("x") }
        stale.walkTopDown().forEach { it.setLastModified(now - 3_600_000) }
        val fresh = File(library, ".new-fresh").apply { mkdirs() }
        val held = File(library, ".new-held").apply { mkdirs(); setLastModified(now - 3_600_000) }
        val heldLock = RandomAccessFile(File(library, ".new-held.lock"), "rw")
        val lock = heldLock.channel.lock()
        Cards.startUp(sessions)
        assertFalse(stale.exists())
        assertTrue(fresh.exists() && held.exists())
        lock.release(); heldLock.close()
        // A linked holder that crashed: settled, its card goes back to the drawer.
        val a = machine("Palm TX", true)
        val b = machine("Partner", false)
        Cards.reconcile(sessions)
        grow("Palm TX card")
        val card = named("Palm TX card")!!
        assertNull(Cards.setLinks(sessions, card.id, listOf(a, b)))
        Cards.reconcile(sessions)
        assertTrue(inDrawer(card))
        assertNull(Cards.cardFileIn(a))
        // Unsettled (a journal beside it): stays held.
        val start = Cards.takeAtStart(sessions, a)
        assertTrue(start.arguments.contains("--card-swap"))
        assertNull(Cards.cardFileIn(File(Cards.entryDir(library, card.id), "")))
        File(a, "sd.frondsd.journal-2").writeText("pending")
        Cards.reconcile(sessions)
        assertTrue(File(a, "sd.frondsd").isFile)
        File(a, "sd.frondsd.journal-2").delete()
        // A running holder is left alone.
        val (file, runLock) = running(a)
        Cards.reconcile(sessions)
        assertTrue(File(a, "sd.frondsd").isFile)
        runLock.release(); file.close()
        // Once stopped, swap set by the take: released.
        Cards.reconcile(sessions)
        assertTrue(inDrawer(card))
    }

    // ---- Serial collisions ----

    // A v6 .frond with a raw SD card inside.
    private fun v6(name: String, sd: ByteArray): File {
        val dir = File(sessions, name.lowercase().replace(' ', '-')).apply { mkdirs() }
        File(dir, "${dir.name}.json").writeText("""{"name":"$name","device":"palm-tx"}""")
        val sections = "meta".toByteArray() + ByteArray(1000) { 'n'.code.toByte() } + ByteArray(3000) { 'm'.code.toByte() } + ByteArray(77) { 's'.code.toByte() }
        val toc = longArrayOf(0, 32, 4, 1000, 0, 3000, 77, sd.size.toLong())
        val head = put32(0x19800819) + put32(6) + "palm-tx".toByteArray().copyOf(32) + toc.fold(ByteArray(0)) { acc, v -> acc + put32(v) }
        val deflater = Deflater(1)
        deflater.setInput(sections + sd)
        deflater.finish()
        val out = ByteArrayOutputStream()
        val buf = ByteArray(1 shl 16)
        while (!deflater.finished()) out.write(buf, 0, deflater.deflate(buf))
        File(dir, "session.frond").writeBytes(head + out.toByteArray())
        return dir
    }

    @Test fun v6CardsAreKeptWithTheirOwnSerials() {
        val sd = ByteArray(32 * 1024) { (it shr 9).toByte() }
        val a = v6("Old TX", sd)
        assertNull(deleteMachine(sessions, a, "Old TX", true))
        assertFalse(a.exists())
        val card = named("Old TX card")!!
        assertEquals(sd.size.toLong(), card.bytes)
        val md5 = java.security.MessageDigest.getInstance("MD5").digest(sd)
        assertArrayEquals(Cards.hex(md5).toByteArray(), card.identity)
        assertArrayEquals(sd, payloadOf(File(Cards.entryDir(library, card.id), "sd.frondsd")))
        // Exported as an image, it is the same bytes.
        val image = File(tmp.root, "old.img")
        assertNull(Cards.exportCardImage(sessions, card.id, image))
        assertArrayEquals(sd, image.readBytes())
        // Two v6 machines duplicated from each other: the second card gets a new serial.
        val b = v6("Twin", sd)
        assertNull(deleteMachine(sessions, b, "Twin", true))
        assertFalse(named("Twin card")!!.identity.contentEquals(card.identity))
        // A cut-off file: refused, left as it is.
        val c = v6("Broken", sd)
        val frond = File(c, "session.frond")
        frond.writeBytes(frond.readBytes().copyOf(200))
        assertTrue(deleteMachine(sessions, c, "Broken", true)!!.endsWith("It wasn't deleted."))
        assertTrue(frond.isFile)
        assertEquals(2, cards().size)
        // No card inside: nothing kept.
        val d = v6("Empty", ByteArray(0))
        assertNull(deleteMachine(sessions, d, "Empty", true))
        assertEquals(2, cards().size)
    }

    @Test fun exportToAnOpenFileHandsTheToolTheDescriptorNotACopy() {
        val card = drawerCard("Games")
        val image = File(tmp.root, "games.img")
        Cards.tool = { args, listener, cancel ->
            // The descriptor number goes in as a word; the fake writes to the file named by the test.
            fakeTool(arrayOf(args[0], args[1], image.path), listener, cancel)
        }
        assertNull(Cards.exportCardImageFd(sessions, card.id, 42))
        assertEquals(listOf("export-fd", File(Cards.entryDir(library, card.id), "sd.frondsd").path, image.path), toolCalls.last())
        assertEquals(512, image.length())
        // A cancel comes back as the usual cancelled word, and the tool is called with the number.
        var seen: List<String>? = null
        Cards.tool = { args, _, _ -> seen = args.toList(); "error cancelled Cancelled." }
        assertEquals(COPY_CANCELLED, Cards.exportCardImageFd(sessions, card.id, 42))
        assertEquals("42", seen!![2])
        // A classic stick can't go this way.
        Cards.tool = { _, _, _ -> "error kind A classic Memory Stick has no raw image." }
        assertEquals("A classic Memory Stick has no raw image.", Cards.exportCardImageFd(sessions, card.id, 42))
    }

    // ---- Import folder as card ----

    private class FakeTree(override val name: String, private val bytes: ByteArray? = null, private val kids: List<FakeTree>? = null,
                           private val declared: Long? = null) : FolderCard.Node {
        override val isDirectory get() = kids != null
        override val size get() = declared ?: bytes?.size?.toLong() ?: 0L
        override fun children(): List<FolderCard.Node> = kids.orEmpty()
        override fun open() = java.io.ByteArrayInputStream(bytes ?: ByteArray(0))
    }

    private fun leftovers() = library.listFiles { f -> f.name.startsWith(".new-") }.orEmpty().size

    @Test fun aFolderBecomesANewCardThroughTheTool() {
        val root = FakeTree("Music", kids = listOf(
            FakeTree("A long name that needs a long entry.txt", "hello".toByteArray()),
            FakeTree("Songs", kids = listOf(FakeTree("Album", kids = listOf(FakeTree("track one.sunvox", ByteArray(200_000) { it.toByte() })))))))
        // The tool is given a FAT image holding the tree; look inside it before the stand-in wraps it.
        var listed = emptyList<String>()
        var size = -1L
        Cards.tool = { args, listener, cancel ->
            if (args[0] == "import") {
                FatImage.open(File(args[1]), false).use { listed = it.list("/").map { e -> e.name }.sorted() + it.list("/Songs/Album").map { e -> e.name } }
                size = File(args[1]).length()
            }
            fakeTool(args, listener, cancel)
        }
        val progress = mutableListOf<Long>()
        var made: String? = null
        assertNull(Cards.importFolderCard(sessions, root, "Music", "sd", 32, { false }, { d, t -> progress += d * 1000 / t }, { made = it }))
        assertEquals(listOf("A long name that needs a long entry.txt", "Songs", "track one.sunvox"), listed)
        assertEquals(32L shl 20, size)
        assertTrue(progress.isNotEmpty() && progress.zipWithNext().all { (a, b) -> a <= b })
        val card = named("Music")!!
        assertEquals(made, card.id)
        assertEquals("sd", card.kind)
        assertEquals("import", toolCalls.last()[0])
        assertEquals("sd", toolCalls.last()[2])
        assertEquals(0, leftovers())
        assertFalse(File(Cards.entryDir(library, card.id), ".work.img").exists())
        // The name is taken now; a classic stick isn't offered for a folder.
        assertEquals("There's already a card called Music.", Cards.importFolderCard(sessions, root, "Music", "sd", 32))
        assertEquals("Choose an SD card or Memory Stick Pro size.", Cards.importFolderCard(sessions, root, "Stick", "ms", 16))
        assertFalse("ms" in FolderCard.kinds())
    }

    @Test fun aFolderThatIsTooBigOrCancelledLeavesNothing() {
        val big = FakeTree("Big", kids = listOf(FakeTree("huge.bin", declared = 40L shl 20)))
        assertEquals("The folder doesn't fit on a 32 MB card.", Cards.importFolderCard(sessions, big, "Big", "sd", 32))
        assertTrue(cards().isEmpty())
        assertEquals(0, leftovers())
        val small = FakeTree("Small", kids = listOf(FakeTree("a.txt", "x".toByteArray())))
        // Cancelled before anything, while the files go in, and while the tool makes the card file.
        assertEquals(COPY_CANCELLED, Cards.importFolderCard(sessions, small, "Small", "sd", 32, { true }))
        assertTrue(cards().isEmpty())
        assertEquals(0, leftovers())
        var calls = 0
        assertEquals(COPY_CANCELLED, Cards.importFolderCard(sessions, small, "Small", "sd", 32, { ++calls > 3 }))
        assertTrue(cards().isEmpty())
        assertEquals(0, leftovers())
        var toolStarted = false
        Cards.tool = { args, listener, cancel -> if (args[0] == "import") toolStarted = true; fakeTool(args, listener, cancel) }
        assertEquals(COPY_CANCELLED, Cards.importFolderCard(sessions, small, "Small", "sd", 32, { toolStarted }))
        assertTrue(toolStarted)
        assertTrue(cards().isEmpty())
        assertEquals(0, leftovers())
        // No room for the raw image, the card file and the files.
        Cards.freeSpace = { 40L shl 20 }
        assertTrue(Cards.importFolderCard(sessions, small, "Small", "sd", 32)!!.startsWith("Not enough free space"))
        assertEquals(0, leftovers())
        // A folder with a name FAT can't hold is refused with the name.
        val odd = FakeTree("Odd", kids = listOf(FakeTree("what?.txt", "x".toByteArray())))
        Cards.freeSpace = { Long.MAX_VALUE / 4 }
        assertEquals("what?.txt can't be used on a card.", Cards.importFolderCard(sessions, odd, "Odd", "sd", 32))
    }

    @Test fun takeOutKeepsTheToolsSerialUnlessTaken() {
        val card = drawerCard("Games")
        assertTrue(Cards.identityUsed(library, card.identity))
        assertFalse(Cards.identityUsed(library, card.identity, except = card.id))
        assertFalse(Cards.identityUsed(library, Cards.newIdentity()))
    }

    // ---- Linked machines taking turns ----

    @Test fun linkedTurns() {
        val a = machine("Palm TX", false)
        val b = machine("Tungsten E2", false, device = "palm-e2")
        val iiix = machine("IIIx", false, device = "palm-iiix")
        val games = drawerCard("Games")
        val moved = mutableListOf<String>()
        assertNull(Cards.setLinks(sessions, games.id, listOf(a, b, iiix), moved = moved))
        assertEquals(games.id, Cards.readSlot(a).link)
        assertEquals(games.id, Cards.readSlot(b).link)
        assertFalse(Cards.readSlot(iiix).present)
        // a starts: takes the card, loads it as a swap with its serial.
        val (fa, la) = running(a)
        val startA = Cards.takeAtStart(sessions, a)
        assertEquals(listOf("--card-swap", "--card-identity", Cards.hex(games.identity)), startA.arguments)
        assertTrue(File(a, "sd.frondsd").isFile)
        assertEquals(a.name, Cards.readCard(Cards.entryDir(library, games.id))!!.machine)
        // b starts while a runs: empty, told where the card is.
        val startB = Cards.takeAtStart(sessions, b)
        assertEquals("Palm TX", startB.heldBy)
        assertEquals(emptyList<String>(), startB.arguments)
        assertNull(Cards.cardFileIn(b))
        // a saves (swap cleared) and closes: the card goes back.
        File(a, "session.frond").writeBytes(frond("palm-tx", listOf(Media(identity = String(games.identity), generation = 1))))
        Cards.clearSwap(a)
        assertFalse(Cards.readSlot(a).swap)
        assertNull(Cards.releaseAtClose(sessions, a))
        la.release(); fa.close()
        assertTrue(inDrawer(games))
        assertTrue(Cards.readSlot(a).swap)
        assertEquals(games.id, Cards.readSlot(a).link)
        // b's next start takes it.
        val startB2 = Cards.takeAtStart(sessions, b)
        assertEquals(listOf("--card-swap", "--card-identity", Cards.hex(games.identity)), startB2.arguments)
        assertTrue(File(b, "sd.frondsd").isFile)
        // A write-protected card says so.
        Cards.writeCard(Cards.entryDir(library, games.id), Cards.readCard(Cards.entryDir(library, games.id))!!.copy(writeProtected = true))
        assertEquals("--card-protected", Cards.launchArguments(sessions, b).last())
        // An unsettled holder keeps it at close.
        File(b, "sd.frondsd.journal-2").writeText("pending")
        assertEquals("The card stays in the machine: its last save isn't finished.", Cards.releaseAtClose(sessions, b))
        assertTrue(File(b, "sd.frondsd").isFile)
        // A machine that holds another card can't be linked; a too-big card isn't linked to the E2.
        val big = drawerCard("Big").let { c ->
            val bigger = c.copy(bytes = 4096L shl 20)
            Cards.writeCard(Cards.entryDir(library, c.id), bigger); bigger
        }
        assertNull(Cards.setLinks(sessions, big.id, listOf(a, b)))
        assertEquals(games.id, Cards.readSlot(b).link)
        assertEquals(games.id, Cards.readSlot(a).link)
    }

    // ---- Card operations ----

    @Test fun cardOperations() {
        val made = mutableListOf<String>()
        assertNull(Cards.makeCard(sessions, "Music", "sd", 128, made = { made += it }))
        assertEquals(listOf("make", "sd", "128"), toolCalls.last().take(3))
        val music = Cards.readCard(Cards.entryDir(library, made.single()))!!
        assertEquals("Music", music.name)
        assertEquals("There's already a card called Music.", Cards.makeCard(sessions, "Music", "sd", 128))
        assertEquals(COPY_CANCELLED, Cards.makeCard(sessions, "Other", "sd", 128, cancelled = { true }))
        assertTrue(library.list()!!.none { it.startsWith(".new-") })
        // Import names it after the file; duplicate gives a new serial; rename and delete.
        val raw = File(tmp.root, "photos.img").apply { writeBytes(ByteArray(1024) { 7 }) }
        assertNull(Cards.importCard(sessions, raw, "sd", "photos"))
        assertArrayEquals(raw.readBytes(), payloadOf(File(Cards.entryDir(library, named("photos")!!.id), "sd.frondsd")))
        assertNull(Cards.duplicateCard(sessions, music.id))
        assertFalse(named("Music 2")!!.identity.contentEquals(music.identity))
        assertEquals("There's already a card called Music 2.", Cards.renameCard(sessions, music.id, "Music 2"))
        assertNull(Cards.renameCard(sessions, music.id, "Songs"))
        assertNull(Cards.deleteCard(sessions, named("Music 2")!!.id))
        assertNull(named("Music 2"))
        // Taken out of a stopped machine; a card in a machine can't be deleted.
        val a = machine("Palm TX", true)
        Cards.reconcile(sessions)
        val tx = named("Palm TX card")!!
        assertEquals("Take the card out of its machine first.", Cards.deleteCard(sessions, tx.id))
        val (f, l) = running(a)
        assertEquals("Palm TX is running. Stop it first.", Cards.takeOutStopped(sessions, tx.id))
        l.release(); f.close()
        assertNull(Cards.takeOutStopped(sessions, tx.id))
        assertTrue(inDrawer(tx))
        assertEquals(Cards.Slot(true, null, null, "Palm TX card", true), Cards.readSlot(a))
    }
}
