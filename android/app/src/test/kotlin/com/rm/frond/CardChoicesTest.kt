package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

// What the Cards screen, New card, Link and the running machine's picker offer (CardChoices).
class CardChoicesTest {
    @get:Rule val tmp = TemporaryFolder()
    private lateinit var sessions: File
    private lateinit var library: File
    private val tx = Device.TX
    private val e2 = Device.E2
    private val tg50 = Device.byId("sony-clie-tg50")!!

    @Before fun setUp() {
        sessions = File(tmp.newFolder("files"), "sessions").apply { mkdirs() }
        library = Cards.libraryFor(sessions)
    }

    // A saved machine: <id>.json, a .frond (only its presence matters here) and slot.json.
    private fun machine(name: String, device: Device, slot: Cards.Slot? = null, cardFile: String? = null): File {
        val dir = File(sessions, name.lowercase().replace(' ', '-')).apply { mkdirs() }
        File(dir, "${dir.name}.json").writeText("""{"name":"$name","device":"${device.id}","created":1,"used":0}""")
        File(dir, "session.frond").writeBytes(ByteArray(16))
        if (slot != null) assertTrue(Cards.writeSlot(dir, slot))
        if (cardFile != null) File(dir, cardFile).writeBytes(ByteArray(64))
        return dir
    }

    // A library card of `mib` MiB (MB for a classic stick), in the drawer or in `holder`.
    private fun card(name: String, kind: String = "sd", mib: Int = 64, holder: File? = null): Cards.Card {
        val bytes = if (kind == "ms") Device.memoryStickPhysicalBytes(mib) / 521 * 512 else mib.toLong() shl 20
        val card = Cards.Card(Cards.newId(), name, kind, Cards.now(), holder?.name, bytes, Cards.newIdentity(), false)
        val dir = Cards.entryDir(library, card.id).apply { mkdirs() }
        assertTrue(Cards.writeCard(dir, card))
        val file = Cards.fileOfKind(kind)
        if (holder == null) File(dir, file).writeBytes(ByteArray(64))
        else {
            File(holder, file).writeBytes(ByteArray(64))
            val slot = Cards.readSlot(holder)
            assertTrue(Cards.writeSlot(holder, slot.copy(present = true, card = card.id, name = name)))
        }
        return card
    }

    // ---- Sizes ----

    @Test fun typesShowWhenSomeDeviceTakesThem() {
        assertEquals(listOf("sd", "ms", "ms-pro"), CardChoices.kindsTaken())
        assertEquals(listOf("sd"), CardChoices.kindsFor(tx))
        assertEquals(listOf("ms", "ms-pro"), CardChoices.kindsFor(tg50))
        assertEquals(listOf("sd"), CardChoices.kindsTaken(listOf(tx, e2)))
        assertEquals(emptyList<String>(), CardChoices.kindsFor(Device.byId("palm-iiix")))
    }

    @Test fun sizesGoSmallestToLargest() {
        val labels = { kind: String, device: Device? -> CardChoices.sizeOptions(kind, device).map { it.label } }
        assertEquals(listOf("32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB", "2 GB"), labels("sd", null))
        assertEquals(listOf("4 MB", "8 MB", "16 MB", "32 MB", "64 MB", "128 MB"), labels("ms", null))
        assertEquals(listOf("256 MB", "512 MB", "1 GB", "2 GB"), labels("ms-pro", null))
        // Classic sticks come before Pro in the type list, so the stick sizes run on into Pro.
        assertEquals(listOf(4, 8, 16, 32, 64, 128, 256, 512, 1024, 1886),
            CardChoices.kindsFor(tg50).flatMap { k -> CardChoices.sizeOptions(k, tg50).map { it.size } })
    }

    @Test fun tooBigSizesAreGreyedForTheMachine() {
        val e2Sizes = CardChoices.sizeOptions("sd", e2)
        assertEquals("2 GB · Too big for Tungsten E2", e2Sizes.last().label)
        assertFalse(e2Sizes.last().enabled)
        assertTrue(e2Sizes.dropLast(1).all { it.enabled })
        assertTrue(CardChoices.sizeOptions("sd", tx).all { it.enabled })
        // A device with only part of the range: sizes it doesn't take for other reasons aren't listed.
        val small = tx.copy(sdMin = 128L shl 20, sdMax = 512L shl 20)
        assertEquals(listOf("128 MB", "256 MB", "512 MB", "1 GB · Too big for Palm TX", "2 GB · Too big for Palm TX"),
            CardChoices.sizeOptions("sd", small).map { it.label })
    }

    @Test fun defaultSizeIs128MbWhenItFits() {
        assertEquals(128, CardChoices.defaultSize(CardChoices.sizeOptions("ms", tg50)))
        assertEquals(128, CardChoices.defaultSize(CardChoices.sizeOptions("sd", e2)))
        assertEquals(256, CardChoices.defaultSize(CardChoices.sizeOptions("ms-pro", tg50)))
        assertNull(CardChoices.defaultSize(emptyList()))
    }

    @Test fun imageKindsFromHeaderAndSize() {
        val none = ByteArray(8)
        assertEquals(listOf("ms"), CardChoices.imageKinds("FRONDMS\u0000".toByteArray(), 100))
        assertEquals(emptyList<String>(), CardChoices.imageKinds("FRONDSD\u0000".toByteArray(), 32L shl 20))
        assertEquals(listOf("sd"), CardChoices.imageKinds(none, 64L shl 20))
        assertEquals(listOf("sd", "ms-pro"), CardChoices.imageKinds(none, 256L shl 20))
        assertEquals(emptyList<String>(), CardChoices.imageKinds(none, (16L shl 20)))
        assertEquals(emptyList<String>(), CardChoices.imageKinds(none, (64L shl 20) + 1))
    }

    // ---- Cards screen ----

    @Test fun rowsSayWhereEachCardIs() {
        val holder = machine("Palm TX", tx)
        val spare = machine("Spare", tx)
        val games = card("Games", holder = holder)
        card("Books")
        val maps = card("Maps")
        assertTrue(Cards.writeSlot(spare, Cards.readSlot(spare).copy(present = true, link = maps.id)))
        assertTrue(Cards.writeSlot(holder, Cards.readSlot(holder).copy(link = games.id)))
        card("Stick", kind = "ms", mib = 16)
        val rows = CardChoices.rows(sessions)
        assertEquals(listOf("Books", "Games", "Maps", "Stick"), rows.map { it.card.name })
        assertEquals(listOf("Not in a machine", "In Palm TX · Linked to Palm TX", "Free · Linked to Spare", "Not in a machine"),
            rows.map { CardChoices.statusText(it) })
        assertEquals(listOf(false, true, false, false), rows.map { it.inMachine })
        val sections = CardChoices.sections(rows)
        assertEquals(listOf("SD cards", "Memory Sticks"), sections.map { it.first })
        assertEquals(listOf("Stick"), sections[1].second.map { it.card.name })
        assertEquals("16 MB", CardChoices.sizeText(rows.last().card))
    }

    @Test fun aRunningHolderShowsAsRunning() {
        val holder = machine("Palm TX", tx)
        card("Games", holder = holder)
        Cards.tryRunLock(holder)!!.use {
            assertTrue(CardChoices.rows(sessions).single().holderRunning)
            // Its own process holds it: not running elsewhere.
            assertFalse(CardChoices.rows(sessions, holder).single().holderRunning)
        }
        assertFalse(CardChoices.rows(sessions).single().holderRunning)
    }

    // ---- Picker ----

    @Test fun pickerGreysCardsInOtherMachinesAndTooBig() {
        val mine = machine("Tungsten", e2)
        val other = machine("Palm TX", tx)
        card("Games", holder = other)
        card("Big", mib = 1886)
        card("Books", mib = 128)
        val here = card("Here", holder = mine)
        card("Stick", kind = "ms", mib = 16)
        val choices = CardChoices.pickerChoices(sessions, mine, e2)
        assertEquals(listOf("No card", "Big · 2 GB · Too big for Tungsten E2", "Books · 128 MB", "Games · 64 MB · In Palm TX", "Here · 64 MB"),
            choices.map { it.text })
        assertEquals(listOf(true, false, true, false, true), choices.map { it.enabled })
        assertEquals(here.id, CardChoices.pickerStart(choices, here.id))
        // A card that can't be picked starts on No card.
        assertNull(CardChoices.pickerStart(choices, choices[1].id))
        // The TG50 sees only sticks.
        assertEquals(listOf("No card", "Stick · 16 MB"), CardChoices.pickerChoices(sessions, machine("Clie", tg50), tg50).map { it.text })
    }

    // ---- Link dialog ----

    @Test fun linkOptionsGreyAsTheDesktopDoes() {
        val games = card("Games", mib = 1886)
        val other = card("Other")
        val tx1 = machine("A TX", tx)
        val e2m = machine("B E2", e2)
        val holding = machine("C TX", tx)
        card("Held", holder = holding)
        val linked = machine("D TX", tx, Cards.Slot(true, link = other.id))
        val running = machine("E TX", tx)
        machine("F Clie", tg50)
        Cards.tryRunLock(running)!!.use {
            val options = CardChoices.linkOptions(sessions, games.id, tick = tx1)
            assertEquals(listOf("A TX · Palm TX", "B E2 · Tungsten E2 · Too big for Tungsten E2", "C TX · Palm TX · Has Held, take it out first",
                "D TX · Palm TX · Linked to Other", "E TX · Palm TX · Running"), options.map { it.text })
            assertEquals(listOf(true, false, false, true, false), options.map { it.enabled })
            assertEquals(listOf(true, false, false, false, false), options.map { it.checked })
            assertEquals(listOf(tx1, e2m, holding, linked, running).map { it.canonicalFile }, options.map { it.dir.canonicalFile })
        }
    }

    // ---- Machine dialogs ----

    @Test fun duplicateChoicesPutLinkFirst() {
        val holder = machine("Palm TX", tx)
        val games = card("Games", holder = holder)
        assertEquals(listOf(CardChoice.LINK, CardChoice.COPY, CardChoice.NONE), CardChoices.duplicateChoices(sessions, holder))
        assertEquals("Games", CardChoices.cardNameOf(sessions, holder))
        // Its linked card held elsewhere: Link or No card.
        val spare = machine("Spare", tx, Cards.Slot(true, link = games.id))
        assertEquals(listOf(CardChoice.LINK, CardChoice.NONE), CardChoices.duplicateChoices(sessions, spare))
        assertEquals("Games", CardChoices.cardNameOf(sessions, spare))
        // No card at all: no question.
        assertEquals(emptyList<CardChoice>(), CardChoices.duplicateChoices(sessions, machine("Empty", tx)))
        assertEquals(listOf("Link", "Copy", "No card"), CardChoices.duplicateChoices(sessions, holder).map { CardChoices.choiceLabel(it) })
        assertEquals("The copy shares Games with Palm TX.", CardChoices.duplicateClause(CardChoice.LINK, "Games", "Games 2", "Palm TX"))
        assertEquals("The copy gets its own card, Games 2.", CardChoices.duplicateClause(CardChoice.COPY, "Games", "Games 2", "Palm TX"))
        assertEquals("The copy has no card.", CardChoices.duplicateClause(CardChoice.NONE, "Games", "Games 2", "Palm TX"))
    }

    @Test fun deleteClauseNamesTheCard() {
        val holder = machine("Palm TX", tx)
        card("Games", holder = holder)
        assertEquals("Its card Games stays in Cards.", CardChoices.deleteClause(sessions, holder))
        assertNull(CardChoices.deleteClause(sessions, machine("Empty", tx)))
        assertEquals("Its card stays in Cards.", CardChoices.deleteClause(sessions, machine("Loose", tx, cardFile = "sd.frondsd")))
    }

    // ---- The running machine's slot ----

    @Test fun takeOutAndPutInMoveTheFile() {
        val dir = machine("Palm TX", tx)
        val games = card("Games", holder = dir)
        val reported = "ab".repeat(32)
        assertNull(Cards.tookOut(sessions, dir, reported, true))
        val back = Cards.readCard(Cards.entryDir(library, games.id))!!
        assertTrue(File(Cards.entryDir(library, games.id), "sd.frondsd").isFile)
        assertFalse(File(dir, "sd.frondsd").exists())
        assertEquals(reported, Cards.hex(back.identity))
        assertTrue(back.writeProtected)
        assertTrue(Cards.readSlot(dir).swap)
        assertNull(Cards.readSlot(dir).card)
        val (card, error) = Cards.toRunning(sessions, dir, games.id)
        assertNull(error)
        assertEquals(games.id, card!!.id)
        assertTrue(File(dir, "sd.frondsd").isFile)
        assertEquals(games.id, Cards.readSlot(dir).card)
        // A refused put-in sends it back.
        Cards.putInFailed(sessions, dir, games.id)
        assertTrue(File(Cards.entryDir(library, games.id), "sd.frondsd").isFile)
        // A card not in Cards yet gets its entry on the way out.
        val loose = machine("Loose", tx, cardFile = "sd.frondsd")
        assertNull(Cards.tookOut(sessions, loose, null, false))
        assertEquals("Loose card", Cards.entries(library).single { it.card.name.startsWith("Loose") }.card.name)
        assertFalse(File(loose, "sd.frondsd").exists())
    }
}
