package com.rm.frond

import java.io.File

// Makes the Zodiac's ROM from Tapwave's Zodiac 1.1 update (the zip or its PRC), with the same C++ as
// the desktop (zodiac-rom/ZodiacRom.cpp). In libfrondcard.so.
object ZodiacRom {
    init {
        System.loadLibrary("frondcard")
    }

    sealed interface Result {
        // The ROM was written to the output file.
        data object Made : Result
        // Not a zip or PRC: use the file as it is.
        data object NotUpdate : Result
        data class Failed(val message: String) : Result
    }

    @JvmStatic private external fun convert(input: String, output: String): String

    fun make(input: File, output: File): Result {
        val reply = convert(input.path, output.path)
        return when {
            reply == "ok" -> Result.Made
            reply == "none" -> Result.NotUpdate
            reply.startsWith("error ") -> Result.Failed(reply.removePrefix("error "))
            else -> Result.Failed("Couldn't read the file.")
        }
    }
}
