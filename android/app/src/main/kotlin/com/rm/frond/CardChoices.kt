package com.rm.frond

import java.io.File

// What the Cards screen, the New card and Link dialogs and the running machine's card picker
// offer, from the device profiles and the library. Plain data, so the JVM tests can check it.
object CardChoices {
    // ---- Types and sizes ----

    fun deviceTakesKind(device: Device, kind: String): Boolean = !device.runs68k && when (kind) {
        "sd" -> device.sdSlot
        "ms" -> device.memoryStick && device.msSizes.isNotEmpty()
        else -> device.memoryStick && device.msProMax >= 256
    }

    // Whether `device` takes the card, and when it doesn't, whether only its size is wrong.
    // Sizes in MiB for SD and Pro, MB for a classic stick.
    fun fits(device: Device, kind: String, size: Int): Boolean = deviceTakesKind(device, kind) && Cards.deviceTakes(device, kind, size)

    fun tooBig(device: Device, kind: String, size: Int): Boolean {
        if (!deviceTakesKind(device, kind) || fits(device, kind, size)) return false
        return when (kind) {
            "sd" -> (size.toLong() shl 20) > device.sdMax
            "ms" -> size > (device.msSizes.maxOrNull() ?: 0)
            else -> size > device.msProMax
        }
    }

    private fun allSizes(kind: String) = when (kind) {
        "sd" -> Device.SD_SIZES
        "ms" -> Device.MEMORY_STICK_SIZES
        else -> Device.MEMORY_STICK_PRO_SIZES
    }

    // Sizes some device takes, smallest first.
    fun sizesFor(kind: String, devices: List<Device> = Device.entries): List<Int> =
        allSizes(kind).filter { size -> devices.any { fits(it, kind, size) } }

    // The kinds some device takes: SD, then classic sticks, then Pro.
    fun kindsTaken(devices: List<Device> = Device.entries): List<String> =
        listOf("sd", "ms", "ms-pro").filter { sizesFor(it, devices).isNotEmpty() }

    // The New card dialog's types: all of them, or the ones `device` takes.
    fun kindsFor(device: Device?, devices: List<Device> = Device.entries): List<String> =
        kindsTaken(devices).filter { device == null || deviceTakesKind(device, it) }

    class SizeOption(val size: Int, val label: String, val enabled: Boolean)

    // The New card sizes for `kind`; for a machine's picker, sizes past what it takes are greyed
    // "Too big for <device>" and sizes it doesn't take for another reason aren't offered.
    fun sizeOptions(kind: String, device: Device?, devices: List<Device> = Device.entries): List<SizeOption> =
        sizesFor(kind, devices).mapNotNull { size ->
            when {
                device == null || fits(device, kind, size) -> SizeOption(size, Device.sizeLabel(size), true)
                tooBig(device, kind, size) -> SizeOption(size, "${Device.sizeLabel(size)} · Too big for ${device.label}", false)
                else -> null
            }
        }

    // 128 MB when it can be had (a stick's default), else the smallest that fits.
    fun defaultSize(options: List<SizeOption>): Int? =
        options.firstOrNull { it.enabled && it.size == 128 }?.size ?: options.firstOrNull { it.enabled }?.size

    // "32 MB", or "Pro · 1 GB" for a Memory Stick Pro.
    fun sizeText(card: Cards.Card): String {
        val size = Device.sizeLabel(Cards.sizeOf(card.kind, card.bytes))
        return if (card.kind == "ms-pro") "Pro · $size" else size
    }

    // What an image file holds from its first bytes and size: "ms" for a Frond stick file, else the
    // raw kinds some device takes at that size (SD and Pro can both fit). Empty when none does.
    fun imageKinds(head: ByteArray, bytes: Long, devices: List<Device> = Device.entries): List<String> {
        val magic = head.copyOf(8)
        if (magic.contentEquals("FRONDMS\u0000".toByteArray(Charsets.US_ASCII))) return listOf("ms")
        if (magic.contentEquals("FRONDSD\u0000".toByteArray(Charsets.US_ASCII))) return emptyList()
        if (bytes <= 0 || bytes % 512 != 0L) return emptyList()
        val kinds = mutableListOf<String>()
        if (devices.any { deviceTakesKind(it, "sd") && bytes in it.sdMin..it.sdMax } && bytes in (32L shl 20)..(1886L shl 20)) kinds += "sd"
        if (bytes % (1 shl 20) == 0L && (bytes shr 20).toInt() in Device.MEMORY_STICK_PRO_SIZES &&
            devices.any { fits(it, "ms-pro", (bytes shr 20).toInt()) }) kinds += "ms-pro"
        return kinds
    }

    // ---- Machines and rows ----

    class Machine(val dir: File, val name: String, val device: Device, val slot: Cards.Slot, val cardFile: String?, val running: Boolean)

    // Saved ARM machines with a card slot, by name. `held` is the caller's own machine, which
    // isn't running elsewhere.
    fun machines(sessions: File, held: File? = null): List<Machine> =
        sessions.listFiles { f -> f.isDirectory && !f.name.startsWith(".") }.orEmpty().mapNotNull { dir ->
            if (sessionFile(dir) == null) return@mapNotNull null
            val device = Cards.machineDevice(dir) ?: return@mapNotNull null
            if (listOf("sd", "ms", "ms-pro").none { deviceTakesKind(device, it) }) return@mapNotNull null
            val own = held != null && held.canonicalFile == dir.canonicalFile
            val running = !own && (Cards.tryRunLock(dir)?.also { it.close() } == null)
            Machine(dir, Cards.machineName(dir), device, Cards.readSlot(dir), Cards.cardFileIn(dir), running)
        }.sortedBy { it.name.lowercase() }

    class Row(
        val card: Cards.Card,
        val inDrawer: Boolean,
        val holder: String?, // the name of the machine it's in
        val holderDir: File?,
        val holderRunning: Boolean,
        val links: List<String>, // names of the machines linked to it
    ) {
        val inMachine: Boolean get() = !inDrawer
    }

    fun rows(sessions: File, held: File? = null): List<Row> {
        val library = Cards.libraryFor(sessions)
        val all = machines(sessions, held)
        return Cards.entries(library).map { e ->
            val links = all.filter { it.slot.link == e.card.id }.map { it.name }
            var holder = if (e.inDrawer) null else all.firstOrNull { it.slot.card == e.card.id && it.cardFile == Cards.fileOfKind(e.card.kind) }
            // A machine that isn't saved yet isn't listed but still holds it.
            val hint = if (!e.inDrawer && holder == null) e.card.machine?.let { File(sessions, it) } else null
            Row(e.card, e.inDrawer, holder?.name ?: hint?.let { Cards.machineName(it) }, holder?.dir ?: hint,
                holder?.running ?: (hint != null && (held == null || held.canonicalFile != hint.canonicalFile) &&
                    Cards.tryRunLock(hint)?.also { it.close() } == null), links)
        }.sortedBy { it.card.name.lowercase() }
    }

    // "In Palm TX", "Free" or "Not in a machine", then the machines a linked card goes with.
    fun statusText(row: Row): String {
        val where = row.holder?.let { "In $it" } ?: if (row.links.isNotEmpty()) "Free" else "Not in a machine"
        return if (row.links.isEmpty()) where else "$where · Linked to ${row.links.joinToString(", ")}"
    }

    // The Cards screen's sections: SD cards, then Memory Sticks (classic and Pro).
    fun sections(rows: List<Row>): List<Pair<String, List<Row>>> = listOf(
        "SD cards" to rows.filter { it.card.kind == "sd" },
        "Memory Sticks" to rows.filter { it.card.kind != "sd" },
    )

    // ---- The running machine's picker ----

    class Choice(val id: String?, val text: String, val enabled: Boolean)

    // No card, then the cards `device` takes; one in another machine greyed "In X", one too big
    // greyed "Too big for <device>".
    fun pickerChoices(sessions: File, machineDir: File, device: Device): List<Choice> {
        val mine = machineDir.canonicalFile
        return listOf(Choice(null, "No card", true)) + rows(sessions, machineDir).mapNotNull { r ->
            if (!deviceTakesKind(device, r.card.kind)) return@mapNotNull null
            val size = Cards.sizeOf(r.card.kind, r.card.bytes)
            val ok = fits(device, r.card.kind, size)
            if (!ok && !tooBig(device, r.card.kind, size)) return@mapNotNull null
            val here = r.holderDir?.canonicalFile == mine
            val text = "${r.card.name} · ${sizeText(r.card)}"
            when {
                !ok -> Choice(r.card.id, "$text · Too big for ${device.label}", false)
                r.holder != null && !here -> Choice(r.card.id, "$text · In ${r.holder}", false)
                else -> Choice(r.card.id, text, true)
            }
        }
    }

    // The choice to start on: the machine's card when it can be picked, else No card.
    fun pickerStart(choices: List<Choice>, current: String?): String? =
        choices.firstOrNull { it.id == current && it.enabled }?.id

    // ---- Link dialog ----

    class LinkOption(val dir: File, val text: String, val enabled: Boolean, val checked: Boolean)

    // Machines that take card `id`'s kind, "Name · Device", greyed when too big, when they hold
    // another card or when they're running. `tick` starts ticked if it can be.
    fun linkOptions(sessions: File, id: String, held: File? = null, tick: File? = null): List<LinkOption> {
        val library = Cards.libraryFor(sessions)
        val card = Cards.readCard(Cards.entryDir(library, id)) ?: return emptyList()
        val names = Cards.entries(library).associate { it.card.id to it.card.name }
        val size = Cards.sizeOf(card.kind, card.bytes)
        return machines(sessions, held).mapNotNull { m ->
            if (!deviceTakesKind(m.device, card.kind)) return@mapNotNull null
            val ok = fits(m.device, card.kind, size)
            if (!ok && !tooBig(m.device, card.kind, size)) return@mapNotNull null
            var text = "${m.name} · ${m.device.label}"
            var enabled = true
            when {
                !ok -> { text += " · Too big for ${m.device.label}"; enabled = false }
                m.cardFile != null && m.slot.card != id -> {
                    text += " · Has ${names[m.slot.card] ?: Cards.kindName(Cards.kindOfFile(m.cardFile))}, take it out first"
                    enabled = false
                }
                m.running -> { text += " · Running"; enabled = false }
                m.slot.link != null && m.slot.link != id -> text += " · Linked to ${names[m.slot.link] ?: "another card"}"
            }
            val ticked = m.slot.link == id || (enabled && tick != null && tick.canonicalFile == m.dir.canonicalFile)
            LinkOption(m.dir, text, enabled, ticked)
        }
    }

    // ---- Machine dialogs ----

    // What Duplicate offers for machine `dir`: nothing when it has no card (the copy is made as
    // before), Link, Copy and No card when it holds one, Link and No card when its linked card is
    // elsewhere. Link comes first.
    fun duplicateChoices(sessions: File, dir: File): List<CardChoice> {
        val library = Cards.libraryFor(sessions)
        val slot = Cards.readSlot(dir)
        val file = Cards.cardFileIn(dir)
        val holds = file != null && slot.card != null && Cards.readCard(Cards.entryDir(library, slot.card)) != null
        return when {
            holds -> listOf(CardChoice.LINK, CardChoice.COPY, CardChoice.NONE)
            file != null -> emptyList() // a card not in Cards yet: copied with the machine
            slot.link != null && Cards.readCard(Cards.entryDir(library, slot.link)) != null -> listOf(CardChoice.LINK, CardChoice.NONE)
            else -> emptyList()
        }
    }

    // The card Duplicate and Delete name: the one in the machine, else the one it's linked to.
    fun cardNameOf(sessions: File, dir: File): String? {
        val library = Cards.libraryFor(sessions)
        val slot = Cards.readSlot(dir)
        val id = (if (Cards.cardFileIn(dir) != null) slot.card else null) ?: slot.link ?: return null
        return Cards.readCard(Cards.entryDir(library, id))?.name
    }

    fun choiceLabel(choice: CardChoice) = when (choice) {
        CardChoice.LINK -> "Link"
        CardChoice.COPY -> "Copy"
        CardChoice.NONE -> "No card"
    }

    // `copyName` is what Copy names the new card ("Games 2").
    fun duplicateClause(choice: CardChoice, card: String, copyName: String, machine: String) = when (choice) {
        CardChoice.LINK -> "The copy shares $card with $machine."
        CardChoice.COPY -> "The copy gets its own card, $copyName."
        CardChoice.NONE -> "The copy has no card."
    }

    // The Delete machine dialog's clause, when the machine holds a library card.
    fun deleteClause(sessions: File, dir: File): String? {
        if (Cards.cardFileIn(dir) == null) return null
        val name = Cards.readSlot(dir).card?.let { Cards.readCard(Cards.entryDir(Cards.libraryFor(sessions), it))?.name }
        return if (name == null) "Its card stays in Cards." else "Its card $name stays in Cards."
    }
}
