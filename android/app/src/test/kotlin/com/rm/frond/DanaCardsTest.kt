package com.rm.frond

import org.json.JSONObject
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.CRC32

class DanaCardsTest {
    @get:Rule val tmp = TemporaryFolder()
    private val dana get() = Device.byId("alphasmart-dana")!!
    private val key = "12345678000000000000000000000000"
    private val second = "23456789000000000000000000000000"
    private val linked get() = "frond-linked-sd 3\n0 sd \"/cards/first.frondsd\" $key 305419896 33554432 1 0 1 2 3 sd 9\n2 sd \"/cards/second.frondsd\" $second 591751049 33554432 0 1 1 4 5 mmc 7\n"
    private val portable get() = "frond-portable-sd 3\n0 sd sd0.frondsd $key 305419896 33554432 1 0 123 sd\n2 sd sd2.frondsd $second 591751049 33554432 0 1 456 mmc\n"
    private fun fixture(manifest: String, portable: Boolean = false, name: String = "alphasmart-dana", version: Long = 8,
                        into: File? = null): File {
        val inner = ByteArray(80)
        fun put(bytes: ByteArray, at: Int, value: Long) { for (n in 0..3) bytes[at+n] = (value shr (8*n)).toByte() }
        put(inner,0,0x19800819); put(inner,4,version)
        name.toByteArray().copyInto(inner,8)
        fun hash(bytes: ByteArray): Long { var hash=2166136261L; for (b in bytes) hash=((hash xor (b.toLong() and 255))*16777619) and 0xffffffffL; return hash }
        val text=manifest.toByteArray()
        val descriptor=ByteArray(12); put(descriptor,0,text.size.toLong()); put(descriptor,4,hash(inner)); put(descriptor,8,hash(text))
        return (into ?: tmp.newFile()).also { file -> file.outputStream().use { out ->
            out.write((if (portable) "FRONDPORTSD1\n" else "FRONDLINKSD1\n").toByteArray()); out.write(descriptor); out.write(text); out.write(inner)
        } }
    }
    @Test fun slotsAndFamiliesUseExactSharedConstraints() {
        assertEquals(listOf(0,2),dana.storageSlots.map { it.id })
        assertEquals(listOf(1,2),dana.storageSlots.map { it.label })
        for (slot in dana.storageSlots) {
            assertEquals(listOf(32,64,128,256,512),slot.families[0].sizes)
            assertEquals(listOf(32,64,128,256),slot.families[1].sizes)
            assertTrue(slot.families[0].accepts((1L shl 30)-(256L shl 10)))
            for (bytes in listOf(1024L shl 20,1886L shl 20,2048L shl 20,33554433L)) assertFalse(slot.families[0].accepts(bytes))
        }
        assertEquals(listOf(32,64,128,256,512),dana.sdSizes)
        assertEquals(listOf(32,64,128,256,512,1024,1886),Device.TX.sdSizes)
        assertTrue(dana.keyboard); assertFalse(dana.infraredKeyboard); assertTrue(dana.infrared)
        assertTrue(Device.TX.infraredKeyboard)
    }
    @Test fun independentCommandPrefixes() {
        assertEquals(listOf("sd-","sd2-"),dana.storageSlots.map { it.commandPrefix })
    }
    @Test fun cardStatePreservesProtectionProtocolAndIoError() {
        val state=LinkedCards.state("ejected $key protected io-error mmc")!!
        assertFalse(state.inserted); assertTrue(state.writeProtected); assertTrue(state.failed); assertEquals("mmc",state.protocol)
        assertEquals("sd",LinkedCards.state("inserted $key writable")!!.protocol)
        for (text in listOf("inserted", "ejected $key maybe", "inserted $key writable mmc mmc", "ejected bad protected")) assertNull(LinkedCards.state(text))
    }
    @Test fun currentSessionVersionIsInspected() {
        // The core saves wrapped Dana sessions at the current session version.
        assertEquals(9L,DanaSession.inspect(fixture(linked,version=9))!!.version)
        assertNull(DanaSession.inspect(fixture(linked,version=5)))
    }
    @Test fun wirelessSessionsKeepTheirDevice() {
        assertEquals("alphasmart-dana",DanaSession.inspect(fixture(linked))!!.device)
        assertEquals("alphasmart-dana-wireless",DanaSession.inspect(fixture(linked,false,"alphasmart-dana-wireless"))!!.device)
        assertNull(DanaSession.inspect(fixture(linked,false,"alphasmart-dana-other")))
        val wireless=Device.byId("alphasmart-dana-wireless")!!
        assertEquals(dana.storageSlots.map { it.id },wireless.storageSlots.map { it.id })
        assertTrue(wireless.isDana() && wireless.keyboard && wireless.infrared)
    }
    @Test fun wrapperInspectionKeepsBothSlotDescriptors() {
        for (portable in listOf(false,true)) {
            val file=fixture(if (portable) this.portable else linked,portable)
            assertTrue(DanaSession.wrapped(file)); val inspected=DanaSession.inspect(file)!!
            assertEquals(portable,inspected.portable); assertEquals(8L,inspected.version)
        }
    }
    @Test fun damagedAndUnsafeWrappersAreRefused() {
        val good=fixture(linked)
        val bytes=good.readBytes(); bytes[bytes.lastIndex]=1; good.writeBytes(bytes); assertNull(DanaSession.inspect(good))
        for (text in listOf(linked.replace("sd 3", "sd 2"), linked.replace("2 sd", "0 sd"), linked.replace(second,key),
                linked.replace(" mmc ", " imaginary "), linked.replace(" 33554432 ", " 1073741824 "))) assertNull(DanaSession.inspect(fixture(text)))
        assertNull(DanaSession.inspect(fixture(portable.replace("sd2.frondsd", "../sd2.frondsd"),true)))
    }
    @Test fun genericCopyAndZipRefuseCardWrappersBeforeWriting() {
        val from=tmp.newFolder(); fixture(linked).copyTo(File(from,"session.frond"))
        val dest=File(tmp.root,"copy")
        assertNotNull(duplicateMachine(tmp.root,from,dest,{ false },CardChoice.COPY)); assertFalse(dest.exists())
        try { zipPackable(from,ByteArrayOutputStream()); fail() } catch (_: java.io.IOException) {}
    }
    @Test fun androidCopyChecksBothSlotsBeforePublishing() {
        assertNull(LinkedCards.sessionCopyError(listOf("none","none")))
        for (states in listOf(listOf("none","ejected $key protected mmc"),listOf("inserted $key writable","none"),
                listOf("none",null),listOf("none","unknown"),listOf("none")))
            assertNotNull(LinkedCards.sessionCopyError(states))
    }
    // ---- The library ----

    private lateinit var sessions: File
    private lateinit var library: File
    private lateinit var old: File
    private val toolCalls = mutableListOf<List<String>>()
    private var refuse: String? = null

    @Before fun setUp() {
        val data = tmp.newFolder("files")
        sessions = File(data, "sessions").apply { mkdirs() }
        library = Cards.libraryFor(sessions)
        old = Cards.oldCardsFor(sessions).apply { mkdirs() }
        Cards.tool = ::fakeTool
    }

    @After fun tearDown() {
        Cards.tool = { args, listener, cancel -> CardTool.run(args, listener, cancel) }
    }

    // move-linked as the card tool does it: the card and its .gen-<file>-* files, never over a file.
    private fun fakeTool(args: Array<String>, listener: CardToolProgress?, cancel: java.util.concurrent.atomic.AtomicBoolean?): String {
        toolCalls += args.toList()
        refuse?.let { return "error unsettled $it" }
        return when (args[0]) {
            "move-linked" -> {
                val from = File(args[1]); val to = File(args[2])
                if (to.exists()) return "error exists The destination already exists."
                for (g in from.parentFile.listFiles { f -> f.name.startsWith(".gen-${from.name}-") }.orEmpty())
                    g.renameTo(File(to.parentFile, ".gen-${to.name}-" + g.name.removePrefix(".gen-${from.name}-")))
                from.renameTo(to); "ok moved"
            }
            "settle-linked" -> {
                val card = File(args[1])
                card.parentFile.listFiles { f -> f.name.startsWith(".gen-${card.name}-") }.orEmpty().forEach { it.delete() }
                "ok settled"
            }
            else -> "error usage bad"
        }
    }

    private fun le(v: Long, n: Int) = ByteArray(n) { (v ushr (8 * it)).toByte() }
    private fun sdCard(payload: ByteArray = ByteArray(512) { 0x5a }): ByteArray {
        val table = le(CRC32().also { it.update(payload) }.value, 4)
        val header = "FRONDSD".toByteArray(Charsets.US_ASCII) + byteArrayOf(0) + le(2, 4) + le(512, 4) +
            le(payload.size.toLong(), 8) + le(0, 4) + le(1, 8) + le(64L + payload.size, 8) + le(1L shl 20, 4) + ByteArray(16)
        val crc = CRC32().also { it.update(header); it.update(table) }.value
        return header.copyOfRange(0, 24) + le(crc, 4) + header.copyOfRange(28, 64) + payload + table
    }
    private fun row(id: Int, path: File, k: String, protocol: String = "sd", protect: Int = 0) =
        "$id ${if (id == 1) "ms" else "sd"} ${Cards.quotePath(path.path)} $k ${k.take(8).toLong(16)} 33554432 $protect 1 1 2 3 $protocol 9\n"
    private fun machine(name: String, device: String = "alphasmart-dana", rows: String? = null): File {
        val dir = File(sessions, name).apply { mkdirs() }
        File(dir, "$name.json").writeText(JSONObject().put("name", name).put("device", device).toString())
        if (rows != null) fixture("frond-linked-sd 3\n$rows", name = device, into = File(dir, "$name.frond"))
        else File(dir, "$name.frond").writeBytes(ByteArray(64))
        return dir
    }
    private fun oldCard(k: String, name: String, family: String = "sdsc", stick: Boolean = false): File {
        val file = File(old, "$k.${if (stick) "frondms" else "frondsd"}").apply { writeBytes(sdCard()) }
        File(old, "$k.json").writeText(JSONObject().put("name", name).put("family", family).toString())
        return file
    }

    @Test fun librarySlotsFollowTheCardFamilies() {
        assertTrue(Cards.hasSlot(dana, 1))
        for (n in 0..1) {
            assertTrue(Cards.deviceTakes(dana, "sd", 512, n)); assertFalse(Cards.deviceTakes(dana, "sd", 1024, n))
            assertTrue(Cards.deviceTakes(dana, "mmc", 256, n)); assertFalse(Cards.deviceTakes(dana, "mmc", 512, n))
            assertFalse(Cards.deviceTakes(dana, "ms", 16, n))
            assertTrue(CardChoices.tooBig(dana, "mmc", 512, n))
        }
        assertEquals(listOf("sd", "mmc"), CardChoices.kindsFor(dana))
        val m125 = Device.byId("palm-m125")!!
        assertFalse(Cards.hasSlot(m125, 1)); assertTrue(Cards.deviceTakes(m125, "mmc", 64))
        assertFalse(Cards.deviceTakes(Device.TX, "mmc", 64)); assertFalse(CardChoices.deviceTakesKind(Device.TX, "mmc"))
        assertEquals(listOf("sd", "mmc", "ms", "ms-pro"), CardChoices.typesTaken())
        assertFalse(Cards.hasSlot(Device.byId("palm-m100")!!, 1))
        val mmc = Cards.Card("x", "Old", "sd", "", null, 64L shl 20, ByteArray(32), false, type = "mmc")
        assertEquals("MMC · 64 MB", CardChoices.sizeText(mmc))
        assertEquals("mmc", Cards.takeKind(mmc))
    }

    @Test fun cardJsonKeepsTypeAndKey() {
        val dir = File(library, "a").apply { mkdirs() }
        val card = Cards.Card("a", "Notes", "sd", "now", null, 512, ByteArray(32), false, type = "mmc", key = key)
        assertTrue(Cards.writeCard(dir, card))
        assertEquals(card, Cards.readCard(dir))
        val o = JSONObject(File(dir, "card.json").readText())
        assertEquals("mmc", o.getString("type")); assertEquals(key, o.getString("key"))
        // A stick has no type; a key that isn't 32 hex isn't kept.
        assertTrue(Cards.writeCard(dir, card.copy(kind = "ms", key = "nope")))
        val stick = JSONObject(File(dir, "card.json").readText())
        assertFalse(stick.has("type")); assertFalse(stick.has("key"))
    }

    @Test fun savedRowsAreRead() {
        val dir = machine("Dana", rows = row(0, File("/x/a \"b\".frondsd"), key) + row(2, File("/x/b.frondsd"), second, "mmc", 1))
        val rows = Cards.linkedRows(File(dir, "Dana.frond"))
        assertTrue(Cards.linkedSession(File(dir, "Dana.frond")))
        assertEquals(listOf(0, 1), rows.map { it.n })
        assertEquals("/x/a \"b\".frondsd", rows[0].path)
        assertEquals(second, rows[1].key); assertEquals("mmc", rows[1].protocol); assertTrue(rows[1].writeProtected)
        assertFalse(Cards.linkedSession(File(machine("Plain"), "Plain.frond")))
    }

    @Test fun attachMakesAKeyApartFromTheOtherSlot() {
        val dir = machine("Dana")
        for ((n, id) in listOf(0 to "one", 1 to "two")) {
            File(library, id).mkdirs()
            Cards.writeCard(File(library, id), Cards.Card(id, id, "sd", "", "Dana", 512, ByteArray(32), n == 1, type = if (n == 1) "mmc" else "sd",
                key = if (n == 0) key else null))
            File(dir, if (n == 0) "sd.frondsd" else "sd2.frondsd").writeBytes(sdCard())
            Cards.writeSlot(dir, Cards.Slot(true, id), n)
        }
        assertEquals("${Cards.quotePath(File(dir, "sd.frondsd").absolutePath)} $key writable sd", Cards.attachArgument(sessions, dir, 0))
        val argument = Cards.attachArgument(sessions, dir, 1)!!
        val made = Cards.readCard(File(library, "two"))!!.key!!
        assertTrue(Cards.validKey(made)); assertNotEquals(key.take(8), made.take(8))
        assertTrue(argument.endsWith(" $made protected mmc"))
        // frond-68k takes only --card-swap.
        Cards.writeSlot(dir, Cards.Slot(true, "one", swap = true))
        assertEquals(listOf("--card-swap"), Cards.launchArguments(sessions, dir, m68k = true))
    }

    @Test fun savedChangesAreSettledBeforeTheDrawer() {
        val file = File(sessions, "Dana/sd.frondsd")
        val dir = machine("Dana", rows = row(0, file, key))
        file.writeBytes(sdCard())
        File(dir, ".gen-sd.frondsd-root").writeText("x")
        Cards.writeSlot(dir, Cards.Slot(true))
        assertNull(Cards.reconcile(sessions))
        val id = Cards.readSlot(dir).card!!
        // The new entry carries the save's key.
        assertEquals(key, Cards.readCard(File(library, id))!!.key)
        Cards.writeSlot(dir, Cards.readSlot(dir).copy(link = id))
        assertNull(Cards.releaseAtClose(sessions, dir))
        assertEquals(listOf("settle-linked", file.path, key), toolCalls.last())
        assertTrue(File(library, "$id/sd.frondsd").isFile)
        assertFalse(File(dir, ".gen-sd.frondsd-root").exists())
    }

    @Test fun moveFromDanaCards() {
        val first = oldCard(key, "Games")
        File(old, ".gen-${first.name}-root").writeText("history")
        val mmc = oldCard(second, "Notes", "legacy-mmc-spi")
        val dir = machine("Dana", rows = row(0, first, key) + row(2, mmc, second, "mmc", 1))
        assertEquals(2, Cards.moveLinkedIn(sessions, dir, old))
        assertTrue(File(dir, "sd.frondsd").isFile); assertTrue(File(dir, "sd2.frondsd").isFile)
        assertTrue(File(dir, ".gen-sd.frondsd-root").isFile)
        assertFalse(first.exists()); assertFalse(File(old, "$key.json").exists())
        val one = Cards.readCard(File(library, Cards.readSlot(dir).card!!))!!
        assertEquals("Games", one.name); assertEquals(key, one.key); assertEquals("sd", one.type); assertEquals("Dana", one.machine)
        val two = Cards.readCard(File(library, Cards.readSlot(dir, 1).card!!))!!
        assertEquals("mmc", two.type); assertTrue(two.writeProtected); assertEquals(second, two.key)
        val log = Cards.movesLog(dir).readLines()
        assertEquals(2, log.size); assertTrue(log[0].endsWith("moved $key to sd.frondsd as Games"))
        // Done once.
        assertEquals(0, Cards.moveLinkedIn(sessions, dir, old))
        // Reconcile keeps them where they are.
        assertNull(Cards.reconcile(sessions))
        assertEquals(one.id, Cards.readSlot(dir).card)
    }

    @Test fun moveFromDanaCardsLeavesWhatItCantMove() {
        val first = oldCard(key, "Games")
        val other = oldCard(second, "Notes")
        val dir = machine("Dana", rows = row(0, first, key) + row(2, other, second))
        // A file already in place is never replaced.
        File(dir, "sd.frondsd").writeBytes(sdCard())
        refuse = "The card has saved changes not written yet."
        assertEquals(0, Cards.moveLinkedIn(sessions, dir, old))
        assertTrue(first.isFile); assertTrue(other.isFile); assertTrue(File(old, "$second.json").isFile)
        val log = Cards.movesLog(dir).readLines()
        assertTrue(log[0].contains("left $key in")); assertTrue(log[0].endsWith("the slot already has a card file"))
        assertTrue(log[1].endsWith("The card has saved changes not written yet."))
        assertNull(Cards.readSlot(dir, 1).card)
    }

    @Test fun moveFromDanaCardsFinishesACutOffMove() {
        val first = oldCard(key, "Games")
        val dir = machine("Dana", rows = row(0, first, key))
        // The tool moved it, then the app stopped before the entry was made.
        first.renameTo(File(dir, "sd.frondsd"))
        assertEquals(1, Cards.moveLinkedIn(sessions, dir, old))
        assertTrue(toolCalls.isEmpty())
        assertEquals("Games", Cards.readCard(File(library, Cards.readSlot(dir).card!!))!!.name)
    }

    @Test fun unlinkedDanaCardsGoToTheDrawer() {
        val linkedCard = oldCard(key, "Games")
        machine("Dana", rows = row(0, linkedCard, key))
        val loose = oldCard(second, "Notes", "legacy-mmc-spi")
        File(old, ".gen-${loose.name}-root").writeText("x")
        val stick = oldCard("3".repeat(32), "Photos", "memory-stick", stick = true)
        val busy = oldCard("4".repeat(32), "Busy")
        // Running the real tool's refusals for one card only.
        Cards.tool = { args, l, c -> if (args[1] == busy.path) "error busy The card is in use." else fakeTool(args, l, c) }
        assertEquals(2, Cards.moveUnlinkedIn(sessions, old))
        assertTrue(linkedCard.isFile); assertTrue(busy.isFile); assertTrue(File(old, "${busy.nameWithoutExtension}.json").isFile)
        assertFalse(loose.exists()); assertFalse(stick.exists()); assertFalse(File(old, "$second.json").exists())
        val entries = Cards.entries(library).associateBy { it.card.name }
        assertEquals(setOf("Notes", "Photos"), entries.keys)
        val notes = entries["Notes"]!!
        assertTrue(notes.inDrawer); assertEquals("mmc", notes.card.type); assertEquals(second, notes.card.key); assertNull(notes.card.machine)
        // Its saved changes were written into it before the move.
        assertTrue(listOf("settle-linked", loose.path, second) in toolCalls)
        assertFalse(File(notes.dir, ".gen-sd.frondsd-root").exists())
        assertEquals("ms", entries["Photos"]!!.card.kind)
        val log = Cards.movesLog(library).readLines()
        assertEquals(3, log.size)
        assertTrue(log.any { it.endsWith("moved $second to the drawer as Notes") })
        assertTrue(log.any { it.contains("left ${busy.nameWithoutExtension} in") && it.endsWith("The card is in use.") })
        // Nothing left behind for the refused card.
        assertEquals(2, library.listFiles { f -> f.isDirectory }!!.size)
    }

    @Test fun cutOffUnlinkedMoveKeepsItsSavedChanges() {
        val loose = oldCard(second, "Notes")
        File(old, ".gen-${loose.name}-root").writeText("root")
        File(old, ".gen-${loose.name}-1").writeText("one")
        // The tool moved one generation into the entry, then failed.
        Cards.tool = { args, l, c ->
            if (args[0] == "move-linked") {
                File(old, ".gen-${loose.name}-1").renameTo(File(File(args[2]).parentFile, ".gen-sd.frondsd-1"))
                "error write Couldn't move the card's saved changes."
            } else "error unsettled The card's saved changes can't be read."
        }
        // Settling fails, so the card stays with its changes.
        assertEquals(0, Cards.moveUnlinkedIn(sessions, old))
        assertTrue(File(old, ".gen-${loose.name}-1").isFile)
        assertTrue(Cards.entries(library).isEmpty())
        // Settled now (the fake drops nothing), the move is cut off part way: the entry stays.
        Cards.tool = { args, l, c ->
            when (args[0]) {
                "settle-linked" -> "ok settled 0"
                else -> {
                    File(old, ".gen-${loose.name}-1").renameTo(File(File(args[2]).parentFile, ".gen-sd.frondsd-1"))
                    "error write Couldn't move the card's saved changes."
                }
            }
        }
        assertEquals(0, Cards.moveUnlinkedIn(sessions, old))
        val entry = Cards.entries(library).single()
        assertTrue(File(entry.dir, ".gen-sd.frondsd-1").isFile); assertFalse(entry.inDrawer)
        assertNull(Cards.reconcile(sessions))
        assertTrue(File(entry.dir, ".gen-sd.frondsd-1").isFile)
        // The next start-up finishes the move into the same entry.
        Cards.tool = ::fakeTool
        assertEquals(1, Cards.moveUnlinkedIn(sessions, old))
        assertFalse(loose.exists())
        val done = Cards.entries(library).single()
        assertEquals(entry.card.id, done.card.id); assertTrue(done.inDrawer)
        assertTrue(File(done.dir, ".gen-sd.frondsd-1").isFile); assertTrue(File(done.dir, ".gen-sd.frondsd-root").isFile)
    }

    @Test fun sharedCardOpensInTheSecondSave() {
        val card = oldCard(key, "Games")
        val first = machine("Dana", rows = row(0, card, key))
        val copy = machine("Dana 2", rows = row(0, card, key))
        assertEquals(1, Cards.moveLinkedIn(sessions, first, old))
        assertFalse(Cards.readSlot(first).swap)
        assertEquals(0, Cards.moveLinkedIn(sessions, copy, old))
        assertTrue(Cards.readSlot(copy).swap); assertNull(Cards.readSlot(copy).card)
        assertEquals(listOf("--card-swap"), Cards.launchArguments(sessions, copy, true))
        assertTrue(Cards.movesLog(copy).readText().contains("is in Cards as Games"))
        // A save outside the sessions folder that links it loads with the slot empty too.
        val outside = File(tmp.root, "outside.frond")
        fixture("frond-linked-sd 3\n" + row(0, card, key), into = outside)
        assertEquals(listOf("--card-swap"), Cards.outsideArguments(outside))
    }
}
