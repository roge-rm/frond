#include "SyncFiles.h"

#include <algorithm>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTimeZone>
#include <QUuid>

using frondSync::PimKind;
using frondSync::PimRecord;

namespace {
constexpr qsizetype textLimit = 32 * 1024 * 1024;
constexpr qint64 dbLimit = 256 * 1024 * 1024;

bool fail(QString& error, const char* message) {
    error = QObject::tr(message);
    return false;
}

QString field(const PimRecord& record, const char* key) {
    const auto it = record.fields.find(key);
    return it == record.fields.end() ? QString{} : QString::fromStdString(it->second);
}

void set(PimRecord& record, const char* key, const QString& value) {
    record.fields.insert_or_assign(key, value.toStdString());
}

QByteArray extraBytes(const frondSync::Bytes& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size()).toBase64();
}

QJsonObject fieldObject(const PimRecord& record) {
    QJsonObject fields;
    for (const auto& [key, value] : record.fields)
        fields.insert(QString::fromStdString(key), QString::fromStdString(value));
    return fields;
}

QJsonObject extraObject(const PimRecord& record) {
    QJsonObject extras;
    for (const auto& [key, value] : record.extras)
        extras.insert(QString::fromStdString(key), QString::fromLatin1(extraBytes(value)));
    return extras;
}

bool decodeObject(const QString& text, QJsonObject& object) {
    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return false;
    object = doc.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!it.value().isString()) return false;
    return true;
}

QString json(const QJsonObject& object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

QString csvCell(QString value) {
    value.replace('"', QStringLiteral("\"\""));
    return '"' + value + '"';
}

// Doubled quotes escape a quote in a quoted field. A newline inside quotes is part of that field.
bool csvRows(QStringView text, QList<QStringList>& rows) {
    QStringList row;
    QString value;
    enum class State { start, field, quoted, closed };
    State state = State::start;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (state == State::quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { value += c; ++i; }
                else state = State::closed;
            } else value += c;
        } else if (c == ',' || c == '\r' || c == '\n') {
            row.append(value);
            value.clear();
            state = State::start;
            if (c != ',') {
                rows.append(row);
                row.clear();
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            }
        } else if (state == State::start && c == '"') {
            state = State::quoted;
        } else {
            if (state == State::closed || c == '"') return false;
            value += c;
            state = State::field;
        }
    }
    if (state == State::quoted) return false;
    if (!row.isEmpty() || state != State::start) {
        row.append(value);
        rows.append(row);
    }
    return true;
}

using NameKey = std::pair<const char*, const char*>;
const std::vector<NameKey>& interchangeFields(PimKind kind) {
    static const std::vector<NameKey> contacts{
        {"ORG", "company"}, {"TITLE", "title"}, {"NOTE", "note"}, {"URL", "webpage"}, {"BDAY", "birthday"}};
    static const std::vector<NameKey> calendar{{"SUMMARY", "summary"}, {"DESCRIPTION", "note"}, {"LOCATION", "location"}};
    return kind == PimKind::contacts ? contacts : calendar;
}

const QStringList& csvHeader() {
    static const QStringList header{"kind", "category", "private", "archived", "fields", "extras"};
    return header;
}

QString rowOf(const PimRecord& record) {
    const QStringList raw{QString::fromLatin1(frondSync::PimKindName(record.kind)),
                          QString::fromStdString(record.category), record.secret ? "1" : "0", record.archived ? "1" : "0",
                          json(fieldObject(record)), json(extraObject(record))};
    QStringList cells;
    for (const auto& cell : raw) cells << csvCell(cell);
    return cells.join(',');
}

// CSV is the lossless export, including fields the Palm codecs keep as opaque bytes.
bool parseCsv(const QString& text, PimKind kind, std::vector<PimRecord>& out, QString& error) {
    QList<QStringList> rows;
    if (!csvRows(text, rows) || rows.isEmpty() || rows.takeFirst() != csvHeader())
        return fail(error, "Choose a Frond CSV file.");
    for (const auto& cells : rows) {
        if (cells.size() != csvHeader().size()) return fail(error, "Invalid CSV row.");
        PimRecord record;
        if (!frondSync::ParsePimKind(cells[0].toStdString(), record.kind) || record.kind != kind)
            return fail(error, "The CSV contains a different kind of record.");
        record.category = cells[1].toStdString();
        record.secret = cells[2] == "1";
        record.archived = cells[3] == "1";
        QJsonObject fields, extras;
        if (!decodeObject(cells[4], fields) || !decodeObject(cells[5], extras))
            return fail(error, "Invalid record in CSV.");
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
            set(record, it.key().toUtf8().constData(), it.value().toString());
        for (auto it = extras.constBegin(); it != extras.constEnd(); ++it) {
            const auto data = QByteArray::fromBase64Encoding(it.value().toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!data) return fail(error, "Invalid record data in CSV.");
            const auto* first = reinterpret_cast<const uint8_t*>(data.decoded.constData());
            record.extras[it.key().toStdString()] = {first, first + data.decoded.size()};
        }
        out.push_back(std::move(record));
    }
    return true;
}

QString escaped(QString text) {
    text.replace("\\", "\\\\").replace(';', "\\;").replace(',', "\\,")
        .replace("\r\n", "\n").replace('\r', '\n').replace('\n', "\\n");
    return text;
}

QStringList splitEscaped(QStringView text, QChar delimiter) {
    QStringList words;
    QString word;
    bool slash = false;
    for (const auto c : text) {
        if (slash) { word += c == 'n' || c == 'N' ? QChar('\n') : c; slash = false; }
        else if (c == '\\') slash = true;
        else if (c == delimiter) { words << word; word.clear(); }
        else word += c;
    }
    if (slash) word += '\\';
    words << word;
    return words;
}

QString unescape(const QString& text) { return splitEscaped(text, QChar()).front(); }

QStringList linesOf(QString text) {
    text.replace("\r\n", "\n").replace('\r', '\n');
    QStringList result;
    for (const auto& line : text.split('\n')) {
        if (!result.isEmpty() && line.size() && (line[0] == ' ' || line[0] == '\t')) result.last() += line.mid(1);
        else if (!line.isEmpty()) result << line;
    }
    return result;
}

QString dateOf(const QString& raw) {
    return QDate::fromString(raw.left(8), "yyyyMMdd").toString(Qt::ISODate);
}

QString clockOf(const QString& raw) {
    return raw.size() >= 13 && raw[8] == 'T' ? QTime::fromString(raw.mid(9, 4), "HHmm").toString("HH:mm") : QString();
}

bool parseInterchange(const QString& text, PimKind kind, std::vector<PimRecord>& out, QString& error) {
    const auto component = kind == PimKind::contacts ? QStringLiteral("VCARD")
                         : kind == PimKind::calendar ? QStringLiteral("VEVENT") : QStringLiteral("VTODO");
    QStringList stack;
    PimRecord record;
    int address = 0, phone = 0;
    bool haveName = false;
    QString endDate, fallbackName;
    for (const auto& line : linesOf(text)) {
        const auto colon = line.indexOf(':');
        if (colon < 0) return fail(error, "Invalid contact or calendar line.");
        const auto parameter = line.left(colon);
        const auto name = parameter.section(';', 0, 0).section('.', -1).toUpper();
        const auto raw = line.mid(colon + 1), value = unescape(raw);
        if (name == "BEGIN") {
            if (!stack.isEmpty() && stack.last() == component && raw.compare("VALARM", Qt::CaseInsensitive) == 0)
                return fail(error, "Calendar files with alarms aren't supported yet.");
            stack << raw.toUpper();
            if (stack.last() == component) {
                record = {};
                record.kind = kind;
                address = phone = 0;
                haveName = false;
                endDate.clear();
                fallbackName.clear();
            }
            continue;
        }
        if (name == "END") {
            if (stack.isEmpty() || stack.takeLast() != raw.toUpper()) return fail(error, "Invalid contact or calendar file.");
            if (raw.compare(component, Qt::CaseInsensitive) == 0) {
                if (kind == PimKind::contacts && !haveName && !fallbackName.isEmpty()) set(record, "firstName", fallbackName);
                if (kind == PimKind::calendar) {
                    const auto start = QDate::fromString(field(record, "date"), Qt::ISODate);
                    const bool timed = !field(record, "startTime").isEmpty();
                    if (!start.isValid()) return fail(error, "An event needs a valid start date.");
                    if (!endDate.isEmpty() && endDate != (timed ? start : start.addDays(1)).toString(Qt::ISODate))
                        return fail(error, "Events covering more than one day aren't supported yet.");
                    if (timed && endDate.isEmpty()) set(record, "endTime", field(record, "startTime"));
                    if (timed && field(record, "endTime") < field(record, "startTime"))
                        return fail(error, "An event ends before it starts.");
                }
                out.push_back(std::move(record));
            }
            continue;
        }
        if (stack.isEmpty() || stack.last() != component) continue;
        const bool contact = kind == PimKind::contacts;
        if (name == "CATEGORIES") record.category = splitEscaped(raw, ',').front().toStdString();
        else if (name == "CLASS") record.secret = value.compare("PUBLIC", Qt::CaseInsensitive) != 0;
        else if (contact && name == "X-FROND-PRIVATE") record.secret = value.compare("TRUE", Qt::CaseInsensitive) == 0;
        else if (contact && name == "N") {
            const auto parts = splitEscaped(raw, ';');
            set(record, "lastName", parts.value(0));
            set(record, "firstName", parts.value(1));
            haveName = !parts.value(0).isEmpty() || !parts.value(1).isEmpty();
        } else if (contact && name == "FN") fallbackName = value;
        else if (contact && (name == "TEL" || name == "EMAIL") && phone < 7) {
            const auto key = "phone" + QByteArray::number(++phone);
            set(record, key, value.startsWith("tel:", Qt::CaseInsensitive) ? value.mid(4) : value);
            const QString type = parameter.toUpper();
            const int label = name == "EMAIL" ? 4 : type.contains("CELL") ? 7 : type.contains("FAX") ? 2 :
                              type.contains("PAGER") ? 6 : type.contains("HOME") ? 1 : 0;
            set(record, ("phoneLabel" + QByteArray::number(phone)), QString::number(label));
        } else if (contact && name == "ADR" && address < 3) {
            ++address;
            const auto parts = splitEscaped(raw, ';');
            const auto suffix = address == 1 ? QByteArray{} : QByteArray::number(address);
            const char* keys[] = {"street", "city", "state", "postalCode", "country"};
            for (int i = 0; i < 5; ++i) set(record, keys[i] + suffix, parts.value(i + 2));
        } else if (!contact && (name == "RRULE" || name == "RDATE" || name == "EXDATE" || name == "RECURRENCE-ID" || name == "DURATION")) {
            return fail(error, "Repeats, exceptions and durations in calendar files aren't supported yet.");
        } else if (!contact && (name == "DTSTART" || name == "DTEND" || name == "DUE" || name == "COMPLETED")) {
            auto date = dateOf(value);
            auto clock = clockOf(value);
            if (parameter.contains(";TZID=", Qt::CaseInsensitive))
                return fail(error, "Calendar files with named time zones aren't supported yet.");
            if (date.isEmpty() || (value.size() > 8 && clock.isEmpty()))
                return fail(error, "Invalid date or time in calendar file.");
            if (!clock.isEmpty() && value.endsWith('Z')) {
                const QDateTime utc(QDate::fromString(date, Qt::ISODate), QTime::fromString(clock, "HH:mm"), QTimeZone(0));
                const auto local = utc.toLocalTime();
                date = local.date().toString(Qt::ISODate);
                clock = local.time().toString("HH:mm");
            }
            if (kind == PimKind::calendar && name == "DTSTART") {
                set(record, "date", date);
                if (!clock.isEmpty()) set(record, "startTime", clock);
            } else if (kind == PimKind::calendar && name == "DTEND") {
                endDate = date;
                if (!clock.isEmpty()) set(record, "endTime", clock);
            } else if (kind == PimKind::tasks && name == "DUE") set(record, "dueDate", date);
            else if (kind == PimKind::tasks && name == "COMPLETED") set(record, "completionDate", date);
        } else if (kind == PimKind::tasks && name == "STATUS") set(record, "complete", value == "COMPLETED" ? "1" : "0");
        else if (kind == PimKind::tasks && name == "PRIORITY") set(record, "priority", value);
        else {
            for (const auto& [prop, key] : interchangeFields(kind)) {
                if (name != QLatin1String(prop)) continue;
                QString decoded = value;
                if (name == "ORG") decoded = splitEscaped(raw, ';').front();
                if (name == "BDAY" && value.size() == 8) decoded = dateOf(value);
                set(record, key, decoded);
                break;
            }
        }
    }
    if (!stack.isEmpty() || out.empty()) return fail(error, "No complete records of this kind in the file.");
    return true;
}

void line(QByteArray& out, const QString& text) {
    const auto encoded = text.toUtf8();
    qsizetype start = 0, length = 75;
    while (encoded.size() - start > length) {
        auto end = start + length;
        while ((static_cast<unsigned char>(encoded[end]) & 0xc0) == 0x80) --end;
        out += encoded.sliced(start, end - start) + "\r\n ";
        start = end;
        length = 74;
    }
    out += encoded.sliced(start) + "\r\n";
}

void property(QByteArray& out, const QString& name, const QString& value) {
    if (!value.isEmpty()) line(out, name + ':' + escaped(value));
}

QString compactDate(QString text) { return text.remove('-'); }

void contactLines(QByteArray& out, const PimRecord& record) {
    out += "BEGIN:VCARD\r\nVERSION:4.0\r\n";
    const auto first = field(record, "firstName"), last = field(record, "lastName");
    auto name = (first + ' ' + last).trimmed();
    if (name.isEmpty()) name = field(record, "company");
    line(out, "FN:" + escaped(name));
    line(out, "N:" + escaped(last) + ';' + escaped(first) + ";;;");
    static const char* labels[] = {"TEL;TYPE=work", "TEL;TYPE=home", "TEL;TYPE=fax", "TEL", "EMAIL",
                                    "TEL;TYPE=main-number", "TEL;TYPE=pager", "TEL;TYPE=cell"};
    for (int i = 1; i <= 7; ++i) {
        const auto n = QByteArray::number(i);
        const auto label = field(record, "phoneLabel" + n).toInt();
        const auto name = QLatin1String(labels[std::clamp(label, 0, 7)]);
        const auto value = field(record, "phone" + n);
        if (label == 4) property(out, name, value);
        else property(out, name + QStringLiteral(";VALUE=text"), value);
    }
    for (int i = 1; i <= 3; ++i) {
        const auto suffix = i == 1 ? QByteArray{} : QByteArray::number(i);
        QStringList parts{{}, {}};
        bool populated = false;
        for (const char* key : {"street", "city", "state", "postalCode", "country"}) {
            const auto value = field(record, key + suffix);
            populated |= !value.isEmpty();
            parts << escaped(value);
        }
        if (populated) line(out, "ADR:" + parts.join(';'));
    }
    for (const auto& [prop, key] : interchangeFields(record.kind)) {
        const auto value = field(record, key);
        property(out, QLatin1String(prop), QLatin1String(prop) == "BDAY" ? compactDate(value) : value);
    }
    property(out, "CATEGORIES", QString::fromStdString(record.category));
    if (record.secret) line(out, "X-FROND-PRIVATE:TRUE");
    out += "END:VCARD\r\n";
}

void calendarLines(QByteArray& out, const PimRecord& record) {
    const bool event = record.kind == PimKind::calendar;
    const auto component = event ? QStringLiteral("VEVENT") : QStringLiteral("VTODO");
    line(out, "BEGIN:" + component);
    line(out, "UID:" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    line(out, "DTSTAMP:" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'"));
    if (event) {
        const auto date = compactDate(field(record, "date"));
        auto start = field(record, "startTime").remove(':'), end = field(record, "endTime").remove(':');
        if (start.isEmpty()) line(out, "DTSTART;VALUE=DATE:" + date);
        else {
            line(out, "DTSTART:" + date + 'T' + start + "00");
            if (end > start) line(out, "DTEND:" + date + 'T' + end + "00");
        }
    } else {
        property(out, "DUE;VALUE=DATE", compactDate(field(record, "dueDate")));
        property(out, "PRIORITY", field(record, "priority"));
        line(out, field(record, "complete") == "1" ? "STATUS:COMPLETED" : "STATUS:NEEDS-ACTION");
    }
    for (const auto& [prop, key] : interchangeFields(record.kind)) property(out, QLatin1String(prop), field(record, key));
    property(out, "CATEGORIES", QString::fromStdString(record.category));
    if (record.secret) line(out, "CLASS:PRIVATE");
    line(out, "END:" + component);
}

bool readBytes(const QString& path, qint64 limit, QByteArray& data, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    if (file.size() > limit) return fail(error, "The file is too large.");
    data = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError) { error = file.errorString(); return false; }
    if (data.size() > limit) return fail(error, "The file is too large.");
    return true;
}

bool save(const QString& path, const QByteArray& data, QString& error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}
}  // namespace

namespace frondSyncFiles {
QString filters(PimKind kind) {
    const auto csv = QObject::tr("Frond CSV files (*.csv)");
    switch (kind) {
        case PimKind::contacts: return csv + ";;" + QObject::tr("vCard files (*.vcf)");
        case PimKind::calendar:
        case PimKind::tasks: return csv + ";;" + QObject::tr("iCalendar files (*.ics)");
        case PimKind::memos: return csv + ";;" + QObject::tr("Text files (*.txt)");
    }
    return csv;
}

QString extension(PimKind kind) {
    switch (kind) {
        case PimKind::contacts: return ".vcf";
        case PimKind::calendar:
        case PimKind::tasks: return ".ics";
        case PimKind::memos: return ".txt";
    }
    return {};
}

bool read(const QString& path, PimKind kind, std::vector<PimRecord>& records, QString& error) {
    QByteArray bytes;
    if (!readBytes(path, textLimit, bytes, error)) return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(bytes);
    if (decoder.hasError()) return fail(error, "The file must use UTF-8 text.");
    const auto suffix = '.' + QFileInfo(path).suffix().toLower();
    std::vector<PimRecord> parsed;
    if (suffix == ".csv") {
        if (!parseCsv(text, kind, parsed, error)) return false;
    } else if (suffix == extension(kind)) {
        if (kind == PimKind::memos) {
            PimRecord record;
            record.kind = kind;
            set(record, "text", text);
            parsed.push_back(std::move(record));
        } else if (!parseInterchange(text, kind, parsed, error)) return false;
    } else return fail(error, "Choose a file for this kind of record.");
    records = std::move(parsed);
    return true;
}

bool write(const QString& path, PimKind kind, const std::vector<PimRecord>& records, QString& error) {
    const auto suffix = '.' + QFileInfo(path).suffix().toLower();
    QByteArray bytes;
    if (suffix == ".csv") {
        bytes = csvHeader().join(',').toUtf8() + "\r\n";
        for (const auto& record : records) bytes += rowOf(record).toUtf8() + "\r\n";
    } else if (suffix == extension(kind)) {
        if (kind == PimKind::memos) {
            if (records.size() != 1) return fail(error, "Select one memo to export to a text file.");
            bytes = field(records.front(), "text").toUtf8();
        } else if (kind == PimKind::contacts) {
            for (const auto& record : records) contactLines(bytes, record);
        } else {
            bytes = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Frond//Sync Manager//EN\r\n";
            for (const auto& record : records) {
                if (kind == PimKind::calendar && !QDate::fromString(field(record, "date"), Qt::ISODate).isValid())
                    return fail(error, "An event needs a valid date to export to iCalendar.");
                if (record.fields.count("repeatType") || record.fields.count("exceptions") ||
                    record.fields.count("alarmAdvance") || record.fields.count("alarmTime"))
                    return fail(error, "Choose CSV to export records with repeats, exceptions or alarms.");
                calendarLines(bytes, record);
            }
            bytes += "END:VCALENDAR\r\n";
        }
    } else return fail(error, "Choose a file for this kind of record.");
    return save(path, bytes, error);
}

bool readDatabase(const QString& path, frondSync::Bytes& bytes, QString& error) {
    QByteArray data;
    if (!readBytes(path, dbLimit, data, error)) return false;
    const auto* first = reinterpret_cast<const uint8_t*>(data.constData());
    bytes.assign(first, first + data.size());
    return true;
}

bool writeDatabase(const QString& path, const frondSync::Bytes& bytes, QString& error) {
    return save(path, QByteArray::fromRawData(reinterpret_cast<const char*>(bytes.data()), bytes.size()), error);
}
}  // namespace frondSyncFiles
