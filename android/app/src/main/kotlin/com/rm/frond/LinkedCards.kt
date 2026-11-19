package com.rm.frond

// frond-68k's card replies, for the Dana, the VZ Palms and the T415, whose cards are linked card
// files attached by path (sd-attach, sd2-attach, ms-attach). The rows, Files and the library go
// through SdCard and Cards as on the ARM devices.
object LinkedCards {
    // Save a Copy carries no card files: every slot has to be empty.
    fun sessionCopyError(states: List<String?>, slots: Int = 2): String? {
        if (states.size != slots || states.any { it == null || (it != "none" && state(it) == null) })
            return if (slots == 2) "Couldn't read both card slots." else "Couldn't read the card slot."
        return if (states.any { it != "none" }) (if (slots == 2) "Take the cards out, then save a copy." else "Take the card out, then save a copy.") else null
    }

    // "inserted|ejected KEY protected|writable [io-error] [mmc]".
    data class State(val inserted: Boolean, val key: String, val writeProtected: Boolean, val protocol: String, val failed: Boolean)
    fun state(text: String): State? {
        val words = text.split(' ')
        if (words.size !in 3..5 || words[0] !in listOf("inserted", "ejected") ||
            !Cards.validKey(words[1]) || words[2] !in listOf("protected", "writable")) return null
        val flags = words.drop(3)
        if (flags.distinct().size != flags.size || flags.any { it !in listOf("io-error", "mmc") }) return null
        return State(words[0] == "inserted", words[1], words[2] == "protected", if ("mmc" in flags) "mmc" else "sd", "io-error" in flags)
    }
}
