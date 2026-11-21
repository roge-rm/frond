package com.rm.frond

import org.json.JSONArray
import org.json.JSONObject
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.time.format.ResolverStyle
import java.util.Base64
import java.util.Locale
import java.util.UUID

internal object SyncFiles {
    private val header = listOf("kind", "category", "private", "archived", "fields", "extras")
    private val dateFormat = DateTimeFormatter.BASIC_ISO_DATE
    private val clockFormat = DateTimeFormatter.ofPattern("HH:mm")
    private val dateTimeFormat = DateTimeFormatter.ofPattern("uuuuMMdd'T'HHmmss").withResolverStyle(ResolverStyle.STRICT)
    private val stampFormat = DateTimeFormatter.ofPattern("yyyyMMdd'T'HHmmss'Z'").withZone(ZoneOffset.UTC)
    private val phones = listOf("TEL;TYPE=work", "TEL;TYPE=home", "TEL;TYPE=fax", "TEL", "EMAIL",
        "TEL;TYPE=main-number", "TEL;TYPE=pager", "TEL;TYPE=cell")
    private val addressFields = listOf("street", "city", "state", "postalCode", "country")
    private val contactFields = mapOf("ORG" to "company", "TITLE" to "title", "NOTE" to "note", "URL" to "webpage", "BDAY" to "birthday")
    private val eventFields = mapOf("SUMMARY" to "summary", "DESCRIPTION" to "note", "LOCATION" to "location")

    fun extension(kind: SyncKind): String = when (kind) {
        SyncKind.CONTACTS -> "vcf"
        SyncKind.CALENDAR, SyncKind.TASKS -> "ics"
        SyncKind.MEMOS -> "txt"
    }

    fun read(name: String, kind: SyncKind, text: String): JSONArray {
        val suffix = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
        require(suffix == "csv" || suffix == extension(kind)) { "Choose a file for this kind of record." }
        return when {
            suffix == "csv" -> readCsv(kind, text)
            kind == SyncKind.MEMOS -> JSONArray().put(record(JSONObject().put("text", text)))
            else -> readInterchange(kind, text)
        }
    }

    fun write(suffix: String, kind: SyncKind, records: List<JSONObject>): String {
        require(suffix == "csv" || suffix == extension(kind)) { "Choose a file for this kind of record." }
        return when {
            suffix == "csv" -> writeCsv(kind, records)
            kind == SyncKind.MEMOS -> {
                require(records.size == 1) { "Select one memo to save as text." }
                records.single().getJSONObject("fields").optString("text")
            }
            else -> writeInterchange(kind, records)
        }
    }

    private fun record(fields: JSONObject = JSONObject(), extras: JSONObject = JSONObject(),
                       category: String = "", secret: Boolean = false, archived: Boolean = false) =
        JSONObject().put("fields", fields).put("extras", extras).put("category", category)
            .put("private", secret).put("archived", archived)

    private fun readCsv(kind: SyncKind, text: String): JSONArray {
        val rows = csvRows(text)
        require(rows.firstOrNull() == header) { "Choose a Frond CSV file." }
        return JSONArray().apply {
            for (row in rows.drop(1)) {
                require(row.size == header.size && row[0] == kind.wire) { "The CSV contains an invalid record or a different kind of record." }
                val fields = JSONObject(row[4])
                require(fields.keys().asSequence().all { fields.get(it) is String }) { "Invalid record in CSV." }
                val encoded = JSONObject(row[5])
                val extras = JSONObject()
                for (key in encoded.keys()) {
                    require(encoded.get(key) is String) { "Invalid record data in CSV." }
                    val bytes = Base64.getDecoder().decode(encoded.getString(key))
                    extras.put(key, bytes.joinToString("") { (it.toInt() and 255).toString(16).padStart(2, '0') })
                }
                put(record(fields, extras, row[1], row[2] == "1", row[3] == "1"))
            }
        }
    }

    private fun writeCsv(kind: SyncKind, records: List<JSONObject>) = buildString {
        append(header.joinToString(",")).append("\r\n")
        for (row in records) {
            val raw = row.optJSONObject("extras") ?: JSONObject()
            val extras = JSONObject()
            for (key in raw.keys()) {
                val hex = raw.getString(key)
                require(hex.length % 2 == 0) { "Invalid record data." }
                val bytes = ByteArray(hex.length / 2) { index -> hex.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
                extras.put(key, Base64.getEncoder().encodeToString(bytes))
            }
            val cells = listOf(kind.wire, row.optString("category"), if (row.optBoolean("private")) "1" else "0",
                if (row.optBoolean("archived")) "1" else "0", row.getJSONObject("fields").toString(), extras.toString())
            append(cells.joinToString(",") { "\"${it.replace("\"", "\"\"")}\"" }).append("\r\n")
        }
    }

    // CRLF separates rows only outside a quoted field. A doubled quote within that field is literal.
    private fun csvRows(text: String): List<List<String>> {
        val rows = mutableListOf<List<String>>()
        val cells = mutableListOf<String>()
        val field = StringBuilder()
        var quoted = false
        var closed = false
        var i = 0
        while (i < text.length) {
            val c = text[i++]
            if (quoted) {
                when {
                    c != '"' -> field.append(c)
                    text.getOrNull(i) == '"' -> { field.append('"'); i++ }
                    else -> { quoted = false; closed = true }
                }
            } else if (c == ',' || c == '\r' || c == '\n') {
                cells += field.toString()
                field.clear()
                closed = false
                if (c != ',') {
                    rows += cells.toList()
                    cells.clear()
                    if (c == '\r' && text.getOrNull(i) == '\n') i++
                }
            } else {
                require(!closed && (c != '"' || field.isEmpty())) { "Invalid quoted CSV field." }
                if (c == '"') quoted = true else field.append(c)
            }
        }
        require(!quoted) { "Invalid quoted CSV field." }
        if (closed || cells.isNotEmpty() || field.isNotEmpty()) rows += cells + field.toString()
        return rows
    }

    private fun escape(value: String) = value.replace("\\", "\\\\").replace(";", "\\;").replace(",", "\\,")
        .replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\\n")

    private fun split(value: String, delimiter: Char? = null): List<String> {
        val result = mutableListOf<String>()
        val word = StringBuilder()
        var escaped = false
        for (c in value) {
            when {
                escaped -> { word.append(if (c == 'n' || c == 'N') '\n' else c); escaped = false }
                c == '\\' -> escaped = true
                c == delimiter -> { result += word.toString(); word.clear() }
                else -> word.append(c)
            }
        }
        if (escaped) word.append('\\')
        result += word.toString()
        return result
    }

    private fun component(kind: SyncKind) = when (kind) {
        SyncKind.CONTACTS -> "VCARD"
        SyncKind.CALENDAR -> "VEVENT"
        SyncKind.TASKS -> "VTODO"
        SyncKind.MEMOS -> error("Memos are plain text.")
    }

    private fun dateTime(value: String, parameter: String): Pair<LocalDate, String> {
        require(!parameter.contains(";TZID=", ignoreCase = true)) { "Calendar files with named time zones aren't supported yet." }
        if (value.length == 8) return LocalDate.parse(value, dateFormat) to ""
        val utc = value.endsWith("Z")
        val time = LocalDateTime.parse(value.removeSuffix("Z"), dateTimeFormat)
        val local = if (utc) time.atOffset(ZoneOffset.UTC).atZoneSameInstant(ZoneId.systemDefault()).toLocalDateTime() else time
        return local.toLocalDate() to local.format(clockFormat)
    }

    private fun readInterchange(kind: SyncKind, text: String): JSONArray {
        val part = component(kind)
        val contact = kind == SyncKind.CONTACTS
        val lines = mutableListOf<StringBuilder>()
        for (line in text.lineSequence().filter(String::isNotEmpty)) {
            if ((line[0] == ' ' || line[0] == '\t') && lines.isNotEmpty()) lines.last().append(line.substring(1))
            else lines.add(StringBuilder(line))
        }
        val rows = JSONArray()
        val stack = ArrayDeque<String>()
        var fields = JSONObject()
        var category = ""
        var secret = false
        var displayName = ""
        var endDate: LocalDate? = null
        var phone = 0
        var address = 0
        for (line in lines) {
            val colon = line.indexOf(":")
            require(colon > 0) { "Invalid contact or calendar line." }
            val parameter = line.substring(0, colon)
            val property = parameter.substringBefore(';').substringAfterLast('.').uppercase(Locale.ROOT)
            val raw = line.substring(colon + 1)
            val value = split(raw).single()
            if (property == "BEGIN") {
                val child = value.uppercase(Locale.ROOT)
                require(child != "VALARM" || stack.lastOrNull() != part) { "Calendar files with alarms aren't supported yet." }
                stack.addLast(child)
                if (child == part) {
                    fields = JSONObject()
                    category = ""; secret = false; displayName = ""; endDate = null; phone = 0; address = 0
                }
                continue
            }
            if (property == "END") {
                val child = value.uppercase(Locale.ROOT)
                require(stack.removeLastOrNull() == child) { "Invalid contact or calendar file." }
                if (child != part) continue
                if (contact && fields.optString("firstName").isEmpty() && fields.optString("lastName").isEmpty())
                    fields.put("firstName", displayName)
                if (kind == SyncKind.CALENDAR) {
                    val date = LocalDate.parse(fields.optString("date"))
                    val start = fields.optString("startTime")
                    if (endDate != null) {
                        require(endDate == if (start.isEmpty()) date.plusDays(1) else date) { "Events covering more than one day aren't supported yet." }
                        val end = fields.optString("endTime")
                        require((start.isEmpty() == end.isEmpty()) && end >= start) { "The event end time is invalid." }
                    } else if (start.isNotEmpty()) fields.put("endTime", start)
                }
                rows.put(record(fields, category = category, secret = secret))
                continue
            }
            if (stack.lastOrNull() != part) continue
            when {
                property == "CATEGORIES" -> category = split(raw, ',').first()
                property == "CLASS" -> secret = !value.equals("PUBLIC", true)
                contact && property == "X-FROND-PRIVATE" -> secret = value.equals("TRUE", true)
                contact && property == "FN" -> displayName = value
                contact && property == "N" -> {
                    val name = split(raw, ';')
                    fields.put("lastName", name.first()).put("firstName", name.getOrNull(1).orEmpty())
                }
                contact && (property == "EMAIL" || property == "TEL") && phone < 7 -> {
                    val label = when {
                        property == "EMAIL" -> 4
                        parameter.contains("CELL", true) -> 7
                        parameter.contains("FAX", true) -> 2
                        parameter.contains("PAGER", true) -> 6
                        parameter.contains("HOME", true) -> 1
                        else -> 0
                    }
                    phone++
                    fields.put("phone$phone", if (value.startsWith("tel:", true)) value.substring(4) else value)
                        .put("phoneLabel$phone", label.toString())
                }
                contact && property == "ADR" && address < 3 -> {
                    val parts = split(raw, ';')
                    val suffix = if (++address == 1) "" else address.toString()
                    addressFields.forEachIndexed { i, key -> fields.put(key + suffix, parts.getOrNull(i + 2).orEmpty()) }
                }
                contact -> contactFields[property]?.let { key ->
                    val decoded = when {
                        property == "ORG" -> split(raw, ';').first()
                        property == "BDAY" && value.length == 8 -> LocalDate.parse(value, dateFormat).toString()
                        else -> value
                    }
                    fields.put(key, decoded)
                }
                property in listOf("RRULE", "RDATE", "EXDATE", "RECURRENCE-ID", "DURATION") ->
                    throw IllegalArgumentException("Repeats, exceptions and durations in calendar files aren't supported yet.")
                property == "DTSTART" && kind == SyncKind.CALENDAR -> {
                    val (date, time) = dateTime(value, parameter)
                    fields.put("date", date.toString())
                    if (time.isNotEmpty()) fields.put("startTime", time)
                }
                property == "DTEND" && kind == SyncKind.CALENDAR -> {
                    val (date, time) = dateTime(value, parameter)
                    endDate = date
                    if (time.isNotEmpty()) fields.put("endTime", time)
                }
                property == "DUE" && kind == SyncKind.TASKS -> fields.put("dueDate", dateTime(value, parameter).first.toString())
                property == "COMPLETED" && kind == SyncKind.TASKS -> fields.put("completionDate", dateTime(value, parameter).first.toString())
                property == "STATUS" && kind == SyncKind.TASKS -> fields.put("complete", if (value.equals("COMPLETED", true)) "1" else "0")
                property == "PRIORITY" && kind == SyncKind.TASKS -> fields.put("priority", value)
                else -> eventFields[property]?.let { fields.put(it, value) }
            }
        }
        require(stack.isEmpty() && rows.length() != 0) { "No complete records of this kind in the file." }
        return rows
    }

    // Fold at the octet boundary without splitting a UTF-8 character.
    private fun StringBuilder.line(text: String) {
        val bytes = text.encodeToByteArray()
        var first = 0
        var limit = 75
        while (bytes.size - first > limit) {
            var last = first + limit
            while ((bytes[last].toInt() and 0xc0) == 0x80) last--
            append(bytes.decodeToString(first, last)).append("\r\n ")
            first = last
            limit = 74
        }
        append(bytes.decodeToString(first)).append("\r\n")
    }

    private fun writeInterchange(kind: SyncKind, records: List<JSONObject>) = buildString {
        val contact = kind == SyncKind.CONTACTS
        val part = component(kind)
        if (!contact) append("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Frond//Sync Manager//EN\r\n")
        for (record in records) {
            val fields = record.getJSONObject("fields")
            fun value(key: String) = fields.optString(key)
            fun prop(name: String, value: String) { if (value.isNotEmpty()) line("$name:${escape(value)}") }
            line("BEGIN:$part")
            if (contact) {
                line("VERSION:4.0")
                val first = value("firstName")
                val last = value("lastName")
                line("FN:${escape("$first $last".trim().ifBlank { value("company") })}")
                line("N:${escape(last)};${escape(first)};;;")
                for (i in 1..7) {
                    val label = value("phoneLabel$i").toIntOrNull()?.coerceIn(0, 7) ?: 0
                    prop(phones[label] + if (label == 4) "" else ";VALUE=text", value("phone$i"))
                }
                for (i in 1..3) {
                    val suffix = if (i == 1) "" else i.toString()
                    val parts = addressFields.map { value(it + suffix) }
                    if (parts.any(String::isNotEmpty)) line("ADR:;;${parts.joinToString(";") { escape(it) }}")
                }
                contactFields.forEach { (name, key) -> prop(name, if (name == "BDAY") value(key).replace("-", "") else value(key)) }
            } else {
                require(listOf("repeatType", "exceptions", "alarmAdvance", "alarmTime").none(fields::has)) {
                    "Choose CSV to export records with repeats, exceptions or alarms."
                }
                line("UID:${UUID.randomUUID()}")
                line("DTSTAMP:${stampFormat.format(Instant.now())}")
                if (kind == SyncKind.CALENDAR) {
                    val date = LocalDate.parse(value("date")).format(dateFormat)
                    val start = value("startTime").replace(":", "")
                    val end = value("endTime").replace(":", "")
                    if (start.isEmpty()) line("DTSTART;VALUE=DATE:$date")
                    else {
                        line("DTSTART:${date}T${start}00")
                        if (end > start) line("DTEND:${date}T${end}00")
                    }
                } else {
                    prop("DUE;VALUE=DATE", value("dueDate").replace("-", ""))
                    prop("PRIORITY", value("priority"))
                    line(if (value("complete") == "1") "STATUS:COMPLETED" else "STATUS:NEEDS-ACTION")
                }
                eventFields.forEach { (name, key) -> prop(name, value(key)) }
            }
            prop("CATEGORIES", record.optString("category"))
            if (record.optBoolean("private")) line(if (contact) "X-FROND-PRIVATE:TRUE" else "CLASS:PRIVATE")
            line("END:$part")
        }
        if (!contact) append("END:VCALENDAR\r\n")
    }
}
