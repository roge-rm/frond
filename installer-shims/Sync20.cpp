#include "PalmConduitAbi.h"
#include "PalmConduitProtocol.h"

#include <cstring>

namespace {

using namespace frondConduit;

HANDLE input, output;
SRWLOCK transport = SRWLOCK_INIT;
struct TransportGuard {
    TransportGuard() { AcquireSRWLockExclusive(&transport); }
    ~TransportGuard() { ReleaseSRWLockExclusive(&transport); }
};
bool connected;

bool ReadExact(void* buffer, size_t length) {
    auto* data = static_cast<uint8_t*>(buffer);
    while (length) {
        DWORD count = 0;
        if (!ReadFile(input, data, DWORD(length), &count, nullptr) || !count) return false;
        data += count;
        length -= count;
    }
    return true;
}

bool WriteExact(const void* buffer, size_t length) {
    const auto* data = static_cast<const uint8_t*>(buffer);
    while (length) {
        DWORD count = 0;
        if (!WriteFile(output, data, DWORD(length), &count, nullptr) || !count) return false;
        data += count;
        length -= count;
    }
    return true;
}

template <typename Parse>
long Call(Operation operation, const Writer& request, Parse parse) {
    TransportGuard lock;
    if (!connected) return disconnected;
    Bytes frame{uint8_t(operation)}, reply;
    frame.insert(frame.end(), request.data.begin(), request.data.end());
    if (!WriteFrame(WriteExact, frame) || !ReadFrame(ReadExact, reply)) {
        connected = false;
        return disconnected;
    }
    Reader answer{reply};
    const auto result = answer.Get();
    if (result == success && answer.ok) parse(answer);
    if (!answer.End()) {
        connected = false;
        return disconnected;
    }
    return long(result);
}

long Call(Operation operation, const Writer& request = {}) {
    return Call(operation, request, [](Reader&) {});
}

long OnDb(Operation operation, BYTE handle) {
    Writer request;
    request.Put(handle);
    return Call(operation, request);
}

std::string_view Text(const char* value, size_t bound) {
    return value ? std::string_view(value, strnlen(value, bound)) : std::string_view{};
}

long Date(uint32_t value) {
    constexpr uint32_t epoch = 2082844800;
    return value ? long(value - epoch) : 0;
}

long ReadRecord(Operation operation, Record& record) {
    if (record.bufferSize && !record.data) return badArgument;
    Writer request;
    request.Put(record.handle);
    if (operation == Operation::readId) request.Put(record.id);
    if (operation == Operation::readIndex) request.Put(record.index);
    return Call(operation, request, [&](Reader& reader) {
        record.id = reader.Get();
        record.index = WORD(reader.Get());
        record.attributes = BYTE(reader.Get());
        record.category = short(reader.Get());
        auto bytes = reader.Blob(maxRecord);
        record.size = DWORD(bytes.size());
        if (const auto amount = std::min<size_t>(bytes.size(), record.bufferSize))
            std::memcpy(record.data, bytes.data(), amount);
    });
}

long Find(Operation operation, const Writer& request, DbInfo& db) {
    db = {};
    return Call(operation, request, [&](Reader& reader) {
        db.base.card = reader.Get();
        db.base.flags = WORD(reader.Get());
        db.base.type = reader.Get();
        const auto name = reader.Text(sizeof(db.base.name) - 1);
        std::memcpy(db.base.name, name.data(), name.size());
        db.base.creator = reader.Get();
        db.base.version = WORD(reader.Get());
        db.base.modNumber = reader.Get();
        db.base.index = WORD(reader.Get());
        db.base.created = Date(reader.Get());
        db.base.modified = Date(reader.Get());
        db.base.backedUp = Date(reader.Get());
        db.base.misc = reader.Get();
        db.records = reader.Get();
        db.totalBytes = reader.Get();
        db.dataBytes = reader.Get();
        db.localId = reader.Get();
        db.openRef = reader.Get();
    });
}

}  // namespace

extern "C" {

void FrondConduitConnect(HANDLE read, HANDLE write) {
    input = read;
    output = write;
    connected = true;
}

long FrondConduitComplete(long status) {
    Writer request;
    request.Put(uint32_t(status));
    return Call(Operation::complete, request);
}

long FrondConduitLog(const char* text) {
    const auto message = Text(text, 1025);
    if (message.size() > 1024) return badArgument;
    Writer request;
    request.Text(message);
    return Call(Operation::log, request);
}

long SyncRegisterConduit(DWORD& handle) {
    const auto result = Call(Operation::registerConduit);
    if (result == success) handle = 1;
    return result;
}

long SyncUnRegisterConduit(DWORD handle) {
    if (handle != 1) return badArgument;
    return Call(Operation::unregisterConduit);
}

long SyncReadUserID(User& user) {
    user = {};
    return Call(Operation::user, {}, [&](Reader& reader) {
        const auto name = reader.Text(sizeof(user.name) - 1);
        std::memcpy(user.name, name.data(), name.size());
        user.nameLength = int(name.size());
        user.lastSync = Date(reader.Get());
        user.lastPc = reader.Get();
        user.id = reader.Get();
        user.viewerId = reader.Get();
    });
}

long SyncOpenDB(const char* name, int card, BYTE& handle, BYTE mode) {
    const auto database = Text(name, 32);
    if (database.empty() || database.size() >= 32 || card < 0 || card > 255) return badArgument;
    Writer request;
    request.Put(card);
    request.Put(mode);
    request.Text(database);
    return Call(Operation::open, request, [&](Reader& reader) { handle = BYTE(reader.Get()); });
}

long SyncDeleteDB(const char* name, int card) {
    const auto database = Text(name, 32);
    if (database.empty() || database.size() >= 32 || card < 0 || card > 255) return badArgument;
    Writer request;
    request.Put(card);
    request.Text(database);
    return Call(Operation::removeDb, request);
}

long SyncCreateDB(CreateDb& db) {
    const auto database = Text(db.name, sizeof(db.name));
    if (database.empty() || database.size() >= sizeof(db.name) || db.flags > 0xffff) return badArgument;
    Writer request;
    request.Put(db.card);
    request.Put(db.creator);
    request.Put(db.type);
    request.Put(db.flags);
    request.Put(db.version);
    request.Text(database);
    return Call(Operation::create, request, [&](Reader& reader) { db.handle = BYTE(reader.Get()); });
}

long SyncCloseDB(BYTE handle) { return OnDb(Operation::close, handle); }
long SyncPurgeDeletedRecs(BYTE handle) { return OnDb(Operation::purgeDeleted, handle); }
long SyncPurgeAllRecs(BYTE handle) { return OnDb(Operation::purgeAll, handle); }
long SyncResetSyncFlags(BYTE handle) { return OnDb(Operation::resetFlags, handle); }
long SyncReadRecordById(Record& record) { return ReadRecord(Operation::readId, record); }
long SyncReadRecordByIndex(Record& record) { return ReadRecord(Operation::readIndex, record); }
long SyncReadNextModifiedRec(Record& record) { return ReadRecord(Operation::readModified, record); }
long SyncAddLogEntry(const char* text) {
    const auto value = Text(text, 1025);
    if (value.size() > 1024) return badArgument;
    Writer request;
    request.Text(value);
    return Call(Operation::deviceLog, request);
}
long SyncYieldCycles(WORD wait) {
    if (wait) SwitchToThread();
    return Call(Operation::check);
}

long SyncGetDBRecordCount(BYTE handle, WORD& count) {
    Writer request;
    request.Put(handle);
    return Call(Operation::count, request, [&](Reader& reader) { count = WORD(reader.Get()); });
}

long SyncWriteRec(Record& record) {
    if (record.category < 0 || record.category > 15 || (record.bufferSize && !record.data)) return badArgument;
    Writer request;
    request.Put(record.handle);
    request.Put(record.id);
    request.Put(record.attributes);
    request.Put(uint32_t(record.category));
    request.Blob(record.data, record.bufferSize);
    return Call(Operation::write, request, [&](Reader& reader) {
        record.id = reader.Get();
        record.size = record.bufferSize;
    });
}

long SyncWriteResourceRec(Resource& resource) {
    if (resource.size && !resource.data) return badArgument;
    resource.reserved = 0;
    Writer request;
    request.Put(resource.handle);
    request.Put(resource.type);
    request.Put(resource.id);
    request.Blob(resource.data, resource.size);
    return Call(Operation::writeResource, request);
}

long SyncDeleteRec(Record& record) {
    if (!record.id) return badArgument;
    Writer request;
    request.Put(record.handle);
    request.Put(record.id);
    return Call(Operation::remove, request);
}

long SyncReadDBAppInfoBlock(BYTE handle, DbBlock& block) {
    if (block.size && !block.data) return badArgument;
    Writer request;
    request.Put(handle);
    return Call(Operation::readAppBlock, request, [&](Reader& reader) {
        auto bytes = reader.Blob(maxRecord);
        block.bytesRead = WORD(bytes.size());
        if (const auto amount = std::min<size_t>(block.size, bytes.size()))
            std::memcpy(block.data, bytes.data(), amount);
    });
}

long SyncWriteDBAppInfoBlock(BYTE handle, DbBlock& block) {
    if (block.bytesRead != block.size || (block.size && !block.data)) return badArgument;
    Writer request;
    request.Put(handle);
    request.Blob(block.data, block.size);
    return Call(Operation::writeAppBlock, request);
}

long SyncFindDbByName(FindName& query, DbInfo& db) {
    const auto name = Text(query.name, 32);
    if (name.empty() || name.size() >= 32 || query.card > 255) return badArgument;
    Writer request;
    request.Put(query.options);
    request.Put(query.card);
    request.Text(name);
    return Find(Operation::findName, request, db);
}

long SyncFindDbByTypeCreator(FindType& query, DbInfo& db) {
    Writer request;
    request.Put(query.options);
    request.Put(query.search);
    request.Put(query.type);
    request.Put(query.creator);
    return Find(Operation::findTypeCreator, request, db);
}

long SyncReadAppPreference(Preference& pref) {
    if (pref.bufferSize < 0 || pref.bufferSize > long(maxRecord) || (pref.bufferSize && !pref.data)) return badArgument;
    Writer request;
    request.Put(pref.creator);
    request.Put(pref.id);
    request.Put(std::min<unsigned>(pref.requested, unsigned(pref.bufferSize)));
    request.Put(pref.saved != FALSE);
    return Call(Operation::readPreference, request, [&](Reader& reader) {
        pref.version = WORD(reader.Get());
        pref.actual = WORD(reader.Get());
        auto bytes = reader.Blob(size_t(pref.bufferSize));
        pref.returned = WORD(bytes.size());
        if (!bytes.empty()) std::memcpy(pref.data, bytes.data(), bytes.size());
    });
}

long SyncWriteAppPreference(Preference& pref) {
    if (pref.bufferSize < 0 || pref.bufferSize > long(maxRecord) || (pref.bufferSize && !pref.data)) return badArgument;
    Writer request;
    request.Put(pref.creator);
    request.Put(pref.id);
    request.Put(pref.version);
    request.Put(pref.saved != FALSE);
    request.Blob(pref.data, size_t(pref.bufferSize));
    return Call(Operation::writePreference, request);
}

long SyncReadFeature(DWORD creator, WORD number, DWORD* feature) {
    if (!feature) return badArgument;
    Writer request;
    request.Put(creator);
    request.Put(number);
    return Call(Operation::readFeature, request, [&](Reader& reader) { *feature = reader.Get(); });
}

long SyncReadSysDateTime(long& time) {
    return Call(Operation::dateTime, {}, [&](Reader& reader) { time = Date(reader.Get()); });
}

long SyncReadSingleCardInfo(CardInfo& card) {
    Writer request;
    request.Put(card.card);
    return Call(Operation::cardInfo, request, [&](Reader& reader) {
        card.version = WORD(reader.Get());
        card.created = Date(reader.Get());
        card.romSize = reader.Get();
        card.ramSize = reader.Get();
        card.freeRam = reader.Get();
        const auto name = reader.Text(sizeof(card.name) - 1);
        const auto manufacturer = reader.Text(sizeof(card.manufacturer) - 1);
        std::memset(card.name, 0, sizeof(card.name));
        std::memset(card.manufacturer, 0, sizeof(card.manufacturer));
        std::memcpy(card.name, name.data(), name.size());
        std::memcpy(card.manufacturer, manufacturer.data(), manufacturer.size());
        card.nameLength = BYTE(name.size());
        card.manufacturerLength = BYTE(manufacturer.size());
        card.romDbs = WORD(reader.Get());
        card.ramDbs = WORD(reader.Get());
        card.reserved = 0;
    });
}

}  // extern "C"
