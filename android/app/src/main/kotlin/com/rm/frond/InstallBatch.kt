package com.rm.frond

// Installing several picked files one after the other, in the order picked.
object InstallBatch {
    class Result(val total: Int, val failures: List<Pair<String, String>>) {
        // Null when everything went in; else the files that didn't and why.
        fun summary(): String? = if (failures.isEmpty()) null else
            (if (total > 1) "${total - failures.size} of $total installed.\n\n" else "") +
                failures.joinToString("\n") { (name, reason) -> if (total > 1) "$name: $reason" else reason }
    }

    // install returns the core's result or throws; a throw only fails that file. progress gets
    // the 1-based number of the file about to go in.
    fun <T> run(items: List<T>, name: (T) -> String, progress: (Int, Int) -> Unit, install: (T) -> Unit): Result {
        val failures = mutableListOf<Pair<String, String>>()
        items.forEachIndexed { i, item ->
            progress(i + 1, items.size)
            try {
                install(item)
            } catch (e: Exception) {
                failures += name(item) to ((e as? PalmDatabases.Failed)?.message ?: "Couldn't read or write the file.")
            }
        }
        return Result(items.size, failures)
    }
}
