#ifndef FROND_SYNC_DLP_H
#define FROND_SYNC_DLP_H

// The desktop side of the Desktop Link Protocol: the desktop sends every request and the
// handheld answers each one. Requests and answers are whole messages on a PacketLink (SLP
// and PADP on a serial line or USB, NetSync framing on TCP). Facts from the Palm OS SDK's
// DLCommon.h; all values are big-endian.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "PalmDb.h"

namespace frondSync {

// Whole messages to and from the handheld.
class PacketLink {
   public:
    virtual ~PacketLink() = default;
    virtual bool Send(const Bytes& message) = 0;
    // Waits for the next message. Network HotSync uses timeoutMs as the read idle limit.
    virtual bool Receive(Bytes& message, int timeoutMs) = 0;
    virtual std::string Name() const = 0;  // serial, netsync
    // Set when the line failed: closed, a timeout or an abort from the handheld.
    std::string error;
};

enum DlpFunction : uint8_t {
    dlpReadUserInfo = 0x10,
    dlpWriteUserInfo,
    dlpReadSysInfo,
    dlpGetSysDateTime,
    dlpSetSysDateTime,
    dlpReadStorageInfo,
    dlpReadDBList,
    dlpOpenDB,
    dlpCreateDB,
    dlpCloseDB,
    dlpDeleteDB,
    dlpReadAppBlock,
    dlpWriteAppBlock,
    dlpReadSortBlock,
    dlpWriteSortBlock,
    dlpReadNextModifiedRec,
    dlpReadRecord,
    dlpWriteRecord,
    dlpDeleteRecord,
    dlpReadResource,
    dlpWriteResource,
    dlpDeleteResource,
    dlpCleanUpDatabase,
    dlpResetSyncFlags,
    dlpCallApplication,
    dlpResetSystem,
    dlpAddSyncLogEntry,
    dlpReadOpenDBInfo,
    dlpMoveCategory,
    dlpProcessRPC,
    dlpOpenConduit,
    dlpEndOfSync,
    dlpResetRecordIndex,
    dlpReadRecordIDList,
    dlpReadNextRecInCategory,
    dlpReadNextModifiedRecInCategory,
    dlpReadAppPreference,
    dlpWriteAppPreference,
    dlpReadNetSyncInfo,
    dlpWriteNetSyncInfo,
    dlpReadFeature,
    dlpFindDB,
    dlpSetDBInfo,
    // DLP 1.4, entries over 64 KB (as named in pilot-link).
    dlpReadResourceEx = 0x64,
};

// DLP error codes in answers.
enum DlpError : uint16_t {
    dlpErrNone = 0,
    dlpErrSystem,
    dlpErrIllegalReq,
    dlpErrMemory,
    dlpErrParam,
    dlpErrNotFound,
    dlpErrNoneOpen,
    dlpErrAlreadyOpen,
    dlpErrTooManyOpen,
    dlpErrAlreadyExists,
    dlpErrOpen,
    dlpErrDeleted,
    dlpErrBusy,
    dlpErrNotSupp,
    dlpErrUnused1,
    dlpErrReadOnly,
    dlpErrSpace,
    dlpErrLimit,
    dlpErrSync,
    dlpErrWrapper,
    dlpErrArgument,
    dlpErrSize,
};

// EndOfSync status.
enum DlpSyncTerm : uint16_t { dlpTermNormal = 0, dlpTermOutOfMemory, dlpTermUserCancel, dlpTermOther, dlpTermIncompatible };

struct DlpArg {
    uint8_t id{0};
    Bytes data;
};

// Builds and parses DLP messages.
Bytes DlpRequest(uint8_t function, const std::vector<DlpArg>& args);
// An answer to function: its error code and arguments. False when it isn't one.
bool DlpParseResponse(const Bytes& message, uint8_t function, uint16_t& error, std::vector<DlpArg>& args);
// Requests the other way (tests stand in for the handheld).
bool DlpParseRequest(const Bytes& message, uint8_t& function, std::vector<DlpArg>& args);
Bytes DlpResponse(uint8_t function, uint16_t error, const std::vector<DlpArg>& args);
std::string DlpErrorText(uint16_t error);

struct DlpTime {
    uint32_t palm{0};  // seconds since 1904, 0 for none
};

struct UserInfo {
    uint32_t userId{0}, viewerId{0}, lastSyncPc{0};
    uint32_t successfulSync{0}, lastSync{0};  // Palm seconds
    std::string name;
    Bytes password;
};

struct SysInfo {
    uint32_t romVersion{0}, localization{0};
    Bytes product;
    uint16_t dlpMajor{1}, dlpMinor{0}, compatMajor{0}, compatMinor{0};
    uint32_t maxRecordSize{0xffff};
};

struct DbInfo {
    uint8_t misc{0};  // 0x80 excluded from sync, 0x40 RAM-based
    uint16_t attributes{0};
    uint32_t type{0}, creator{0};
    uint16_t version{0};
    uint32_t modNumber{0};
    uint32_t created{0}, modified{0}, backedUp{0};
    uint16_t index{0};
    std::string name;
};

struct DlpRecord {
    uint32_t uid{0};
    uint8_t flags{0}, category{0};
    Bytes data;
};

struct DlpCardInfo {
    uint16_t version{0}, romDbs{0}, ramDbs{0};
    uint32_t created{0}, romSize{0}, ramSize{0}, freeRam{0};
    std::string name, manufacturer;
};

struct DlpFindInfo {
    uint8_t card{0};
    uint32_t localId{0}, openRef{0}, records{0}, totalBytes{0}, dataBytes{0};
    DbInfo db;
};

struct OpenDbInfo {
    uint16_t records{0};
};

// The desktop's DLP calls. Each returns the handheld's error code, or a negative value when
// the line failed (see LinkError()).
class DlpClient {
   public:
    static constexpr int defaultTimeoutMs = 20000;
    explicit DlpClient(PacketLink& link, int timeoutMs = defaultTimeoutMs) : link(link), timeoutMs(timeoutMs) {}

    // One request and its answer.
    int Call(uint8_t function, const std::vector<DlpArg>& args, std::vector<DlpArg>& answer);
    const std::string& LinkError() const { return link.error; }

    int ReadSysInfo(SysInfo& info);
    int ReadUserInfo(UserInfo& info);
    // modFlags: 0x80 user ID, 0x40 sync PC, 0x20 sync date, 0x10 name, 0x08 viewer ID.
    int WriteUserInfo(const UserInfo& info, uint8_t modFlags);
    int OpenConduit();
    int EndOfSync(uint16_t status);
    int AddSyncLogEntry(const std::string& text);
    int GetSysDateTime(uint32_t& palm);
    int SetSysDateTime(uint32_t palm);
    // flags: 0x80 RAM, 0x40 ROM, 0x20 multiple (DLP 1.2). Fills out with the DBs from start on.
    int ReadDBList(uint8_t flags, uint16_t card, uint16_t start, std::vector<DbInfo>& out);
    // mode: 0x80 read, 0x40 write, 0x20 exclusive, 0x10 secret.
    int OpenDB(uint16_t card, uint8_t mode, const std::string& name, uint8_t& handle);
    int CreateDB(uint32_t creator, uint32_t type, uint16_t card, uint16_t flags, uint16_t version, const std::string& name,
                 uint8_t& handle);
    int CloseDB(uint8_t handle, uint8_t flags = 0);  // flags 0x80 update backup date, 0x40 update mod date
    int DeleteDB(uint16_t card, const std::string& name);
    int ReadOpenDBInfo(uint8_t handle, OpenDbInfo& info);
    int ReadAppBlock(uint8_t handle, Bytes& out);
    int WriteAppBlock(uint8_t handle, const Bytes& data);
    int ReadSortBlock(uint8_t handle, Bytes& out);
    int WriteSortBlock(uint8_t handle, const Bytes& data);
    int ReadRecordByIndex(uint8_t handle, uint16_t index, PalmRecord& out);
    int ReadPimRecordByIndex(uint8_t handle, uint16_t index, DlpRecord& out);
    int ReadNextModifiedRecord(uint8_t handle, DlpRecord& out);
    int ResetRecordIndex(uint8_t handle);
    int DeleteRecord(uint8_t handle, uint32_t uid);
    int CleanUpDatabase(uint8_t handle);
    int ResetSyncFlags(uint8_t handle);
    int ReadRecordIds(uint8_t handle, std::vector<uint32_t>& out);
    // Falls back to ReadResourceEx when the legacy read hits the 64 KB limit on a DLP 1.4 handheld.
    int ReadResourceByIndex(uint8_t handle, uint16_t index, PalmRecord& out);
    int ReadResourceEx(uint8_t handle, uint16_t index, PalmRecord& out);
    int WriteRecord(uint8_t handle, uint8_t flags, const PalmRecord& record, uint32_t& newUid);
    int WritePimRecord(uint8_t handle, const DlpRecord& record, uint32_t& newUid);
    int WriteResource(uint8_t handle, const PalmRecord& resource);
    int ResetSystem();
    int ReadFeature(uint32_t creator, uint16_t number, uint32_t& value);
    int ReadConduitRecord(uint8_t handle, uint32_t idOrIndex, bool byId, bool modified, uint16_t& index, DlpRecord& out);
    int PurgeRecords(uint8_t handle);
    int WriteConduitRecord(uint8_t handle, const DlpRecord& record, uint32_t& newUid);
    int FindByName(uint8_t options, uint8_t card, const std::string& name, DlpFindInfo& info);
    int FindByTypeCreator(uint8_t options, uint8_t search, uint32_t type, uint32_t creator, DlpFindInfo& info);
    int ReadCardInfo(uint8_t card, DlpCardInfo& info);
    int ReadAppPreference(uint32_t creator, uint16_t id, uint16_t requested, bool backedUp,
                          uint16_t& version, uint16_t& actual, Bytes& data);
    int WriteAppPreference(uint32_t creator, uint16_t id, uint16_t version, bool backedUp, const Bytes& data);


    // Set by ReadSysInfo: the handheld has DLP 1.4 and takes entries over 64 KB.
    bool LargeEntries() const { return largeEntryMax > 0xffff; }

   private:
    PacketLink& link;
    int timeoutMs;
    uint32_t largeEntryMax{0};
};

}  // namespace frondSync

#endif
