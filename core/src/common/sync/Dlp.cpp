#include "Dlp.h"

#include <cstring>

namespace frondSync {

namespace {

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Argument bodies are built with these.
struct Out {
    Bytes b;
    Out& U8(uint8_t v) { return b.push_back(v), *this; }
    Out& U16(uint16_t v) { return U8(uint8_t(v >> 8)).U8(uint8_t(v)); }
    Out& U32(uint32_t v) { return U16(uint16_t(v >> 16)).U16(uint16_t(v)); }
    Out& Raw(const Bytes& d) { return b.insert(b.end(), d.begin(), d.end()), *this; }
    Out& Cstr(const std::string& s) { return b.insert(b.end(), s.begin(), s.end()), U8(0); }
    Out& Time(uint32_t palm) {
        if (!palm) return U32(0).U32(0);
        int y, mo, d, h, mi, s;
        PalmTimeParts(palm, y, mo, d, h, mi, s);
        return U16(uint16_t(y)).U8(uint8_t(mo)).U8(uint8_t(d)).U8(uint8_t(h)).U8(uint8_t(mi)).U8(uint8_t(s)).U8(0);
    }
};

uint32_t ReadTime(const uint8_t* p) {
    const int year = Get16(p);
    if (!year) return 0;
    return PalmTime(year, p[2], p[3], p[4], p[5], p[6]);
}

const DlpArg* Find(const std::vector<DlpArg>& args, uint8_t id) {
    for (const auto& a : args)
        if (a.id == id) return &a;
    return nullptr;
}

// Arguments after a header. Each is tiny {id, size8}, small {id|0x80, 0, size16} or long
// {id|0x40, 0, size32}.
bool ParseArgs(const uint8_t* p, size_t size, unsigned count, std::vector<DlpArg>& args) {
    args.clear();
    size_t at = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (at + 2 > size) return false;
        const uint8_t first = p[at];
        size_t length, header;
        if (first & 0x80) {
            if (at + 4 > size) return false;
            length = Get16(p + at + 2), header = 4;
        } else if (first & 0x40) {
            if (at + 6 > size) return false;
            length = Get32(p + at + 2), header = 6;
        } else {
            length = p[at + 1], header = 2;
        }
        if (at + header + length > size) return false;
        args.push_back({uint8_t(first & 0x3f), Bytes(p + at + header, p + at + header + length)});
        at += header + length;
    }
    return true;
}

void PutArgs(Bytes& out, const std::vector<DlpArg>& args) {
    for (const auto& a : args) {
        const size_t n = a.data.size();
        if (n <= 0xff) {
            out.push_back(a.id), out.push_back(uint8_t(n));
        } else if (n <= 0xffff) {
            out.push_back(uint8_t(a.id | 0x80)), out.push_back(0), out.push_back(uint8_t(n >> 8)), out.push_back(uint8_t(n));
        } else {
            out.push_back(uint8_t(a.id | 0x40)), out.push_back(0);
            for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(n >> s));
        }
        out.insert(out.end(), a.data.begin(), a.data.end());
    }
}

std::string CString(const uint8_t* p, size_t max) { return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), max)); }

}  // namespace

Bytes DlpRequest(uint8_t function, const std::vector<DlpArg>& args) {
    Bytes out{function, uint8_t(args.size())};
    PutArgs(out, args);
    return out;
}

Bytes DlpResponse(uint8_t function, uint16_t error, const std::vector<DlpArg>& args) {
    Bytes out{uint8_t(function | 0x80), uint8_t(args.size()), uint8_t(error >> 8), uint8_t(error)};
    PutArgs(out, args);
    return out;
}

bool DlpParseResponse(const Bytes& m, uint8_t function, uint16_t& error, std::vector<DlpArg>& args) {
    if (m.size() < 4 || m[0] != (function | 0x80)) return false;
    error = Get16(&m[2]);
    return ParseArgs(m.data() + 4, m.size() - 4, m[1], args);
}

bool DlpParseRequest(const Bytes& m, uint8_t& function, std::vector<DlpArg>& args) {
    if (m.size() < 2 || (m[0] & 0x80)) return false;
    function = m[0];
    return ParseArgs(m.data() + 2, m.size() - 2, m[1], args);
}

std::string DlpErrorText(uint16_t error) {
    static const char* names[] = {"none", "system error", "illegal request", "out of memory", "bad parameter", "not found",
                                  "no database open", "database already open", "too many open databases", "already exists",
                                  "couldn't open the database", "record deleted", "record busy", "not supported", "unused",
                                  "read-only", "not enough space", "limit exceeded", "sync cancelled", "bad argument wrapper",
                                  "argument missing", "bad argument size"};
    if (error < sizeof(names) / sizeof(names[0])) return names[error];
    return "error " + std::to_string(error);
}

int DlpClient::Call(uint8_t function, const std::vector<DlpArg>& args, std::vector<DlpArg>& answer) {
    if (!link.Send(DlpRequest(function, args))) return -1;
    Bytes reply;
    if (!link.Receive(reply, timeoutMs)) return -1;
    uint16_t error = 0;
    if (!DlpParseResponse(reply, function, error, answer)) {
        // The size and first bytes, for the log.
        static const char hex[] = "0123456789abcdef";
        std::string start;
        for (size_t i = 0; i < reply.size() && i < 24; ++i) start += hex[reply[i] >> 4], start += hex[reply[i] & 15];
        link.error = "The handheld's answer to DLP call " + std::to_string(function) + " was garbled (" +
                     std::to_string(reply.size()) + " bytes: " + start + ").";
        return -1;
    }
    return error;
}

int DlpClient::ReadSysInfo(SysInfo& info) {
    std::vector<DlpArg> answer;
    // DLP 1.4 as the caller's version, as later desktops send.
    const int rc = Call(dlpReadSysInfo, {{0x20, Out().U16(1).U16(4).b}}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.size() < 10) return dlpErrSize;
    info.romVersion = Get32(&a->data[0]);
    info.localization = Get32(&a->data[4]);
    const size_t product = a->data[9];
    info.product.assign(a->data.begin() + 10, a->data.begin() + std::min(a->data.size(), 10 + product));
    if (const DlpArg* v = Find(answer, 0x21); v && v->data.size() >= 12) {
        info.dlpMajor = Get16(&v->data[0]), info.dlpMinor = Get16(&v->data[2]);
        info.compatMajor = Get16(&v->data[4]), info.compatMinor = Get16(&v->data[6]);
        info.maxRecordSize = Get32(&v->data[8]);
    }
    const bool dlp14 = info.dlpMajor > 1 || (info.dlpMajor == 1 && info.dlpMinor >= 4);
    largeEntryMax = dlp14 ? info.maxRecordSize : 0;
    return 0;
}

int DlpClient::ReadUserInfo(UserInfo& info) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadUserInfo, {}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.size() < 30) return dlpErrSize;
    const uint8_t* p = a->data.data();
    info.userId = Get32(p), info.viewerId = Get32(p + 4), info.lastSyncPc = Get32(p + 8);
    info.successfulSync = ReadTime(p + 12), info.lastSync = ReadTime(p + 20);
    const size_t nameLength = p[28], passwordLength = p[29];
    if (30 + nameLength + passwordLength > a->data.size()) return dlpErrSize;
    info.name = CString(p + 30, nameLength);
    info.password.assign(p + 30 + nameLength, p + 30 + nameLength + passwordLength);
    return 0;
}

int DlpClient::WriteUserInfo(const UserInfo& info, uint8_t modFlags) {
    std::string name = info.name.substr(0, 40);
    Out o;
    o.U32(info.userId).U32(info.viewerId).U32(info.lastSyncPc).Time(info.lastSync).U8(modFlags).U8(uint8_t(name.size() + 1)).Cstr(name);
    std::vector<DlpArg> answer;
    return Call(dlpWriteUserInfo, {{0x20, o.b}}, answer);
}

int DlpClient::OpenConduit() {
    std::vector<DlpArg> answer;
    return Call(dlpOpenConduit, {}, answer);
}

int DlpClient::EndOfSync(uint16_t status) {
    std::vector<DlpArg> answer;
    return Call(dlpEndOfSync, {{0x20, Out().U16(status).b}}, answer);
}

int DlpClient::AddSyncLogEntry(const std::string& text) {
    std::vector<DlpArg> answer;
    return Call(dlpAddSyncLogEntry, {{0x20, Out().Cstr(text).b}}, answer);
}

int DlpClient::GetSysDateTime(uint32_t& palm) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpGetSysDateTime, {}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.size() < 8) return dlpErrSize;
    palm = ReadTime(a->data.data());
    return 0;
}

int DlpClient::SetSysDateTime(uint32_t palm) {
    std::vector<DlpArg> answer;
    return Call(dlpSetSysDateTime, {{0x20, Out().Time(palm).b}}, answer);
}

int DlpClient::ReadDBList(uint8_t flags, uint16_t card, uint16_t start, std::vector<DbInfo>& out) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadDBList, {{0x20, Out().U8(flags).U8(uint8_t(card)).U16(start).b}}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.size() < 4) return dlpErrSize;
    const uint8_t* p = a->data.data();
    const size_t count = p[3];
    size_t at = 4;
    for (size_t i = 0; i < count; ++i) {
        if (at + 44 > a->data.size()) return dlpErrSize;
        const uint8_t* e = p + at;
        const size_t total = e[0];
        if (total < 44 || at + total > a->data.size()) return dlpErrSize;
        DbInfo info;
        info.misc = e[1], info.attributes = Get16(e + 2), info.type = Get32(e + 4), info.creator = Get32(e + 8);
        info.version = Get16(e + 12), info.modNumber = Get32(e + 14);
        info.created = ReadTime(e + 18), info.modified = ReadTime(e + 26), info.backedUp = ReadTime(e + 34);
        info.index = Get16(e + 42);
        info.name = CString(e + 44, total - 44);
        out.push_back(info);
        at += total;
    }
    return 0;
}

int DlpClient::OpenDB(uint16_t card, uint8_t mode, const std::string& name, uint8_t& handle) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpOpenDB, {{0x20, Out().U8(uint8_t(card)).U8(mode).Cstr(name).b}}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.empty()) return dlpErrSize;
    handle = a->data[0];
    return 0;
}

int DlpClient::CreateDB(uint32_t creator, uint32_t type, uint16_t card, uint16_t flags, uint16_t version, const std::string& name,
                        uint8_t& handle) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpCreateDB,
                        {{0x20, Out().U32(creator).U32(type).U8(uint8_t(card)).U8(0).U16(flags).U16(version).Cstr(name).b}}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.empty()) return dlpErrSize;
    handle = a->data[0];
    return 0;
}

int DlpClient::CloseDB(uint8_t handle, uint8_t flags) {
    std::vector<DlpArg> answer;
    if (flags) return Call(dlpCloseDB, {{0x22, Out().U8(handle).U8(flags).b}}, answer);
    return Call(dlpCloseDB, {{0x20, Out().U8(handle).b}}, answer);
}

int DlpClient::DeleteDB(uint16_t card, const std::string& name) {
    std::vector<DlpArg> answer;
    return Call(dlpDeleteDB, {{0x20, Out().U8(uint8_t(card)).U8(0).Cstr(name).b}}, answer);
}

int DlpClient::ReadOpenDBInfo(uint8_t handle, OpenDbInfo& info) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadOpenDBInfo, {{0x20, Out().U8(handle).b}}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.size() < 2) return dlpErrSize;
    info.records = Get16(a->data.data());
    return 0;
}

namespace {
int ReadBlock(DlpClient& c, uint8_t function, uint8_t handle, Bytes& out) {
    std::vector<DlpArg> answer;
    const int rc = c.Call(function, {{0x20, Out().U8(handle).U8(0).U16(0).U16(0xffff).b}}, answer);
    out.clear();
    if (rc == dlpErrNotFound) return 0;  // no block
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.size() < 2) return dlpErrSize;
    out.assign(a->data.begin() + 2, a->data.end());
    return 0;
}
}  // namespace

int DlpClient::ReadAppBlock(uint8_t handle, Bytes& out) { return ReadBlock(*this, dlpReadAppBlock, handle, out); }
int DlpClient::ReadSortBlock(uint8_t handle, Bytes& out) { return ReadBlock(*this, dlpReadSortBlock, handle, out); }

int DlpClient::WriteAppBlock(uint8_t handle, const Bytes& data) {
    std::vector<DlpArg> answer;
    return Call(dlpWriteAppBlock, {{0x20, Out().U8(handle).U8(0).U16(uint16_t(data.size())).Raw(data).b}}, answer);
}

int DlpClient::WriteSortBlock(uint8_t handle, const Bytes& data) {
    std::vector<DlpArg> answer;
    return Call(dlpWriteSortBlock, {{0x20, Out().U8(handle).U8(0).U16(uint16_t(data.size())).Raw(data).b}}, answer);
}

namespace {

// The legacy read commands have 16-bit lengths. Leave room for the DLP wrappers so a
// nearly 64 KiB resource does not overrun the Palm's reply buffer.
// An entry over 64 KB reports its size modulo 64 KB but the Palm still sends what was asked for,
// so it is read to its end while the 16-bit offset reaches: up to 0xf000 + 0xf000 bytes. The end
// must match the reported size modulo 64 KB. Anything longer is dlpErrLimit, never a short copy.
int ReadEntry(DlpClient& dlp, bool resource, uint8_t handle, uint32_t key, PalmRecord& out,
              uint8_t* recordFlags = nullptr, uint16_t* recordIndex = nullptr, bool byId = false) {
    constexpr uint16_t maxChunk = 0xf000;
    const uint8_t function = resource ? dlpReadResource : dlpReadRecord;
    const uint8_t argument = resource || byId ? 0x20 : 0x21;
    PalmRecord entry;
    size_t total = 0;
    bool wrapped = false;  // the size field wrapped: total is unknown until a short reply
    for (size_t offset = 0;; offset = entry.data.size()) {
        if (offset > 0xffff) return dlpErrLimit;
        // One byte over what should be left shows an entry whose real size is 64 KB more.
        const uint16_t request = offset && !wrapped ? uint16_t(total - offset + 1) : maxChunk;
        std::vector<DlpArg> answer;
        Out data;
        data.U8(handle).U8(0);
        if (byId) data.U32(key);
        else data.U16(uint16_t(key));
        data.U16(uint16_t(offset)).U16(request);
        const int rc = dlp.Call(function, {{argument, data.b}}, answer);
        if (rc) return rc;
        const DlpArg* arg = Find(answer, 0x20);
        if (!arg || arg->data.size() < 10) return dlpErrSize;
        const uint8_t* header = arg->data.data();
        const size_t reported = Get16(header + (resource ? 8 : 6));
        const size_t count = arg->data.size() - 10;
        if (count > request) return dlpErrLimit;
        if (offset) {
            if (reported != (total & 0xffff) || (resource ? Get32(header) != entry.type || Get16(header + 4) != entry.id
                                                          : (Get32(header) & 0xffffff) != entry.uid))
                return dlpErrSize;
            if (!wrapped && count == request) wrapped = true;
            else if (!wrapped && count != total - offset) return dlpErrSize;
            else if (wrapped && count < request) {
                wrapped = false;
                total = offset + count;
                if ((total & 0xffff) != reported) return dlpErrSize;
            }
        } else {
            total = reported;
            if (!resource && recordFlags && (header[8] & recDeleted) && count == 0) total = 0;
            if (count > reported) {
                wrapped = true;
                total = reported;  // the low 16 bits
            } else if (count != std::min(total, size_t(request))) {
                return dlpErrSize;
            }
            if (resource) {
                entry.type = Get32(header);
                entry.id = Get16(header + 4);
            } else {
                entry.uid = Get32(header) & 0xffffff;
                entry.attributes = uint8_t((header[8] & 0xf0) | (header[9] & 0x0f));
                if (recordFlags) *recordFlags = header[8];
                if (recordIndex) *recordIndex = Get16(header + 4);
            }
        }
        entry.data.insert(entry.data.end(), arg->data.begin() + 10, arg->data.end());
        if (!wrapped && entry.data.size() == total) {
            out = std::move(entry);
            return 0;
        }
    }
}

}  // namespace

int DlpClient::ReadRecordByIndex(uint8_t handle, uint16_t index, PalmRecord& out) {
    return ReadEntry(*this, false, handle, index, out);
}

int DlpClient::ReadPimRecordByIndex(uint8_t handle, uint16_t index, DlpRecord& out) {
    PalmRecord record;
    uint8_t flags = 0;
    const int rc = ReadEntry(*this, false, handle, index, record, &flags);
    if (!rc) out = DlpRecord{record.uid, flags, uint8_t(record.attributes & recCategoryMask), std::move(record.data)};
    return rc;
}

int DlpClient::ReadNextModifiedRecord(uint8_t handle, DlpRecord& out) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadNextModifiedRec, {{0x20, {handle}}}, answer);
    if (rc) return rc;
    const DlpArg* arg = Find(answer, 0x20);
    if (!arg || arg->data.size() < 10) return dlpErrSize;
    const uint8_t* p = arg->data.data();
    if (!(p[8] & recDeleted) && arg->data.size() != size_t(Get16(p + 6)) + 10) return dlpErrSize;
    out = DlpRecord{Get32(p) & 0xffffff, p[8], uint8_t(p[9] & 15), Bytes(arg->data.begin() + 10, arg->data.end())};
    return 0;
}

int DlpClient::ResetRecordIndex(uint8_t handle) {
    std::vector<DlpArg> answer;
    return Call(dlpResetRecordIndex, {{0x20, {handle}}}, answer);
}

int DlpClient::DeleteRecord(uint8_t handle, uint32_t uid) {
    std::vector<DlpArg> answer;
    return Call(dlpDeleteRecord, {{0x20, Out().U8(handle).U8(0).U32(uid).b}}, answer);
}

int DlpClient::CleanUpDatabase(uint8_t handle) {
    std::vector<DlpArg> answer;
    return Call(dlpCleanUpDatabase, {{0x20, {handle}}}, answer);
}

int DlpClient::ResetSyncFlags(uint8_t handle) {
    std::vector<DlpArg> answer;
    return Call(dlpResetSyncFlags, {{0x20, {handle}}}, answer);
}

int DlpClient::ReadRecordIds(uint8_t handle, std::vector<uint32_t>& out) {
    constexpr uint16_t page = 1024;
    std::vector<uint32_t> ids;
    for (;;) {
        std::vector<DlpArg> answer;
        const int rc = Call(dlpReadRecordIDList,
                            {{0x20, Out().U8(handle).U8(0).U16(uint16_t(ids.size())).U16(page).b}}, answer);
        if (rc == dlpErrNotFound) break;
        if (rc) return rc;
        const DlpArg* arg = Find(answer, 0x20);
        if (!arg || arg->data.size() < 2) return dlpErrSize;
        const uint16_t count = Get16(arg->data.data());
        if (count > page || arg->data.size() != 2 + 4u * count || ids.size() + count > 65535) return dlpErrSize;
        for (size_t i = 0; i < count; ++i) ids.push_back(Get32(arg->data.data() + 2 + 4 * i) & 0xffffff);
        if (!count || ids.size() == 65535) break;
    }
    out = std::move(ids);
    return 0;
}

int DlpClient::ReadResourceByIndex(uint8_t handle, uint16_t index, PalmRecord& out) {
    const int rc = ReadEntry(*this, true, handle, index, out);
    if (rc != dlpErrLimit || !LargeEntries()) return rc;
    // Over 64 KB: the legacy reply's 16-bit size can't hold it. Read it again in 32-bit pieces.
    return ReadResourceEx(handle, index, out);
}

int DlpClient::ReadResourceEx(uint8_t handle, uint16_t index, PalmRecord& out) {
    // Request: handle, 0, index, u32 offset, u32 count. Reply: type, id, index, u32 total size, data.
    constexpr uint32_t maxChunk = 0x40000;
    const uint32_t chunk = std::min(maxChunk, largeEntryMax);
    PalmRecord entry;
    uint32_t total = 0;
    for (uint32_t offset = 0;; offset = uint32_t(entry.data.size())) {
        const uint32_t request = offset ? std::min(chunk, total - offset) : chunk;
        std::vector<DlpArg> answer;
        const int rc = Call(dlpReadResourceEx, {{0x20, Out().U8(handle).U8(0).U16(index).U32(offset).U32(request).b}}, answer);
        if (rc) return rc;
        const DlpArg* arg = Find(answer, 0x20);
        if (!arg || arg->data.size() < 12) return dlpErrSize;
        const uint8_t* header = arg->data.data();
        const uint32_t reported = Get32(header + 8);
        const size_t count = arg->data.size() - 12;
        if (count > request || offset + count > reported) return dlpErrSize;
        if (offset) {
            if (reported != total || Get32(header) != entry.type || Get16(header + 4) != entry.id || count == 0)
                return dlpErrSize;
        } else {
            total = reported;
            if (count != std::min(total, request)) return dlpErrSize;
            entry.type = Get32(header);
            entry.id = Get16(header + 4);
        }
        entry.data.insert(entry.data.end(), arg->data.begin() + 12, arg->data.end());
        if (entry.data.size() == total) {
            out = std::move(entry);
            return 0;
        }
    }
}

int DlpClient::WriteRecord(uint8_t handle, uint8_t flags, const PalmRecord& r, uint32_t& newUid) {
    std::vector<DlpArg> answer;
    Out o;
    // Attributes carry only the deleted, dirty, busy, secret and archived bits; the category
    // goes on its own.
    o.U8(handle).U8(uint8_t(flags | 0x80)).U32(r.uid).U8(uint8_t(r.attributes & 0xf8)).U8(uint8_t(r.attributes & 0x0f)).Raw(r.data);
    const int rc = Call(dlpWriteRecord, {{0x20, o.b}}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    newUid = a && a->data.size() >= 4 ? Get32(a->data.data()) : 0;
    return 0;
}

int DlpClient::WritePimRecord(uint8_t handle, const DlpRecord& record, uint32_t& newUid) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpWriteRecord, {{0x20, Out().U8(handle).U8(0x80).U32(record.uid)
        .U8(uint8_t(record.flags & recSecret)).U8(record.category).Raw(record.data).b}}, answer);
    if (rc) return rc;
    const DlpArg* arg = Find(answer, 0x20);
    if (!arg || arg->data.size() < 4) return dlpErrSize;
    newUid = Get32(arg->data.data()) & 0xffffff;
    return newUid ? 0 : dlpErrSize;
}

int DlpClient::WriteResource(uint8_t handle, const PalmRecord& r) {
    std::vector<DlpArg> answer;
    return Call(dlpWriteResource, {{0x20, Out().U8(handle).U8(0).U32(r.type).U16(r.id).U16(uint16_t(r.data.size())).Raw(r.data).b}},
                answer);
}

int DlpClient::ResetSystem() {
    std::vector<DlpArg> answer;
    return Call(dlpResetSystem, {}, answer);
}

int DlpClient::ReadFeature(uint32_t creator, uint16_t number, uint32_t& value) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadFeature, {{0x20, Out().U32(creator).U16(number).b}}, answer);
    if (rc) return rc;
    const DlpArg* a = Find(answer, 0x20);
    if (!a || a->data.size() < 4) return dlpErrSize;
    value = Get32(a->data.data());
    return 0;
}

int DlpClient::ReadConduitRecord(uint8_t handle, uint32_t idOrIndex, bool byId, bool modified, uint16_t& index,
                                 DlpRecord& out) {
    if (!modified) {
        PalmRecord record;
        uint8_t flags = 0;
        const int rc = ReadEntry(*this, false, handle, idOrIndex, record, &flags, &index, byId);
        if (!rc) out = {record.uid, flags, uint8_t(record.attributes & recCategoryMask), std::move(record.data)};
        return rc;
    }
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadNextModifiedRec, {{0x20, {handle}}}, answer);
    if (rc) return rc;
    const auto* reply = Find(answer, 0x20);
    if (!reply || reply->data.size() < 10 || reply->data.size() > 0xffff + 10u) return dlpErrSize;
    const auto* data = reply->data.data();
    if (!(data[8] & recDeleted) && reply->data.size() != 10u + Get16(data + 6)) return dlpErrSize;
    index = Get16(data + 4);
    out = {Get32(data) & 0xffffff, data[8], uint8_t(data[9] & 15), Bytes(data + 10, data + reply->data.size())};
    return dlpErrNone;
}

int DlpClient::WriteConduitRecord(uint8_t handle, const DlpRecord& record, uint32_t& newUid) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpWriteRecord, {{0x20, Out().U8(handle).U8(0x80).U32(record.uid)
        .U8(uint8_t(record.flags & 0xd8)).U8(record.category).Raw(record.data).b}}, answer);
    if (rc) return rc;
    const auto* response = Find(answer, 0x20);
    if (!response || response->data.size() < 4) return dlpErrSize;
    newUid = Get32(response->data.data()) & 0xffffff;
    return newUid ? dlpErrNone : dlpErrSize;
}

int DlpClient::PurgeRecords(uint8_t handle) {
    std::vector<DlpArg> answer;
    return Call(dlpDeleteRecord, {{0x20, Out().U8(handle).U8(0x80).U32(0).b}}, answer);
}

namespace {

int FindConduitDb(DlpClient& dlp, uint8_t argument, const Bytes& request, DlpFindInfo& out) {
    std::vector<DlpArg> answer;
    const int rc = dlp.Call(dlpFindDB, {{argument, request}}, answer);
    if (rc) return rc;
    const auto* basic = Find(answer, 0x20);
    if (!basic || basic->data.size() < 55) return dlpErrSize;
    const auto* bytes = basic->data.data();
    const auto* data = bytes + 10;
    const auto size = basic->data.size() - 10;
    if (data[0] < 45 || data[0] > size) return dlpErrSize;
    out = {};
    out.card = bytes[0];
    out.localId = Get32(bytes + 2);
    out.openRef = Get32(bytes + 6);
    out.db.misc = data[1];
    out.db.attributes = Get16(data + 2);
    out.db.type = Get32(data + 4);
    out.db.creator = Get32(data + 8);
    out.db.version = Get16(data + 12);
    out.db.modNumber = Get32(data + 14);
    out.db.created = ReadTime(data + 18);
    out.db.modified = ReadTime(data + 26);
    out.db.backedUp = ReadTime(data + 34);
    out.db.index = Get16(data + 42);
    out.db.name = CString(data + 44, data[0] - 44);
    if (out.db.name.empty() || out.db.name.size() > 31) return dlpErrSize;
    if (const auto* sizes = Find(answer, 0x21)) {
        if (sizes->data.size() < 12) return dlpErrSize;
        out.records = Get32(sizes->data.data());
        out.totalBytes = Get32(sizes->data.data() + 4);
        out.dataBytes = Get32(sizes->data.data() + 8);
    }
    return dlpErrNone;
}

}  // namespace

int DlpClient::FindByName(uint8_t options, uint8_t card, const std::string& name, DlpFindInfo& info) {
    return FindConduitDb(*this, 0x20, Out().U8(options | 0x80).U8(card).Cstr(name).b, info);
}

int DlpClient::FindByTypeCreator(uint8_t options, uint8_t search, uint32_t type, uint32_t creator, DlpFindInfo& info) {
    return FindConduitDb(*this, 0x22, Out().U8(options | 0x80).U8(search).U32(type).U32(creator).b, info);
}

int DlpClient::ReadCardInfo(uint8_t card, DlpCardInfo& info) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadStorageInfo, {{0x20, {card, 0}}}, answer);
    if (rc) return rc;
    const auto* reply = Find(answer, 0x20);
    if (!reply || reply->data.size() < 30 || !reply->data[3]) return dlpErrSize;
    const auto* data = reply->data.data() + 4;
    const size_t name = data[24], manufacturer = data[25];
    if (data[1] != card || data[0] < 26 + name + manufacturer || data[0] > reply->data.size() - 4 ||
        name > 32 || manufacturer > 32)
        return dlpErrSize;
    info = {};
    info.version = Get16(data + 2);
    info.created = ReadTime(data + 4);
    info.romSize = Get32(data + 12);
    info.ramSize = Get32(data + 16);
    info.freeRam = Get32(data + 20);
    info.name = CString(data + 26, name);
    info.manufacturer = CString(data + 26 + name, manufacturer);
    if (const auto* counts = Find(answer, 0x21)) {
        if (counts->data.size() < 4) return dlpErrSize;
        info.romDbs = Get16(counts->data.data());
        info.ramDbs = Get16(counts->data.data() + 2);
    }
    return dlpErrNone;
}

int DlpClient::ReadAppPreference(uint32_t creator, uint16_t id, uint16_t requested, bool backedUp,
                                 uint16_t& version, uint16_t& actual, Bytes& data) {
    std::vector<DlpArg> answer;
    const int rc = Call(dlpReadAppPreference,
                        {{0x20, Out().U32(creator).U16(id).U16(requested).U8(backedUp ? 0x80 : 0).U8(0).b}}, answer);
    if (rc) return rc;
    const auto* reply = Find(answer, 0x20);
    if (!reply || reply->data.size() < 6) return dlpErrSize;
    const auto* bytes = reply->data.data();
    version = Get16(bytes);
    actual = Get16(bytes + 2);
    const uint16_t sent = Get16(bytes + 4);
    if (sent > requested || sent > actual || reply->data.size() != 6u + sent) return dlpErrSize;
    data.assign(bytes + 6, bytes + 6 + sent);
    return dlpErrNone;
}

int DlpClient::WriteAppPreference(uint32_t creator, uint16_t id, uint16_t version, bool backedUp, const Bytes& data) {
    if (data.size() > 0xffff) return dlpErrSize;
    std::vector<DlpArg> answer;
    return Call(dlpWriteAppPreference,
                {{0x20, Out().U32(creator).U16(id).U16(version).U16(uint16_t(data.size())).U8(backedUp ? 0x80 : 0)
                            .U8(0).Raw(data).b}}, answer);
}

}  // namespace frondSync
