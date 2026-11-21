#ifndef FROND_SYNC_PALM_DB_H
#define FROND_SYNC_PALM_DB_H

// A whole Palm database in memory and its PDB/PRC file form, the format HotSync backups and
// installs use: a 78-byte header, the record or resource list, then the app info block, the
// sort info block and the data, all big-endian. Times are seconds since 1904-01-01 in the
// handheld's local time, as Palm OS keeps them.

#include <cstdint>
#include <string>
#include <vector>

namespace frondSync {

using Bytes = std::vector<uint8_t>;

enum : uint16_t {
    dbResource = 0x0001,
    dbReadOnly = 0x0002,
    dbAppInfoDirty = 0x0004,
    dbBackup = 0x0008,
    dbOkToInstallNewer = 0x0010,
    dbResetAfterInstall = 0x0020,
    dbCopyPrevention = 0x0040,
    dbStream = 0x0080,
    dbHidden = 0x0100,
    dbLaunchableData = 0x0200,
    dbRecyclable = 0x0400,
    dbBundle = 0x0800,
    dbOpen = 0x8000,
};

enum : uint8_t {
    recDeleted = 0x80,
    recDirty = 0x40,
    recBusy = 0x20,
    recSecret = 0x10,
    recArchived = 0x08,
    recCategoryMask = 0x0f,
};

struct PalmRecord {
    uint32_t uid{0};     // records: 24-bit unique ID
    uint8_t attributes{0};  // records: flags and category
    uint32_t type{0};    // resources
    uint16_t id{0};      // resources
    Bytes data;
};

struct PalmDb {
    std::string name;  // up to 31 bytes, the handheld's encoding
    uint16_t attributes{0};
    uint16_t version{0};
    uint32_t created{0}, modified{0}, backedUp{0};
    uint32_t modNumber{0};
    uint32_t type{0}, creator{0};
    uint32_t uniqueIdSeed{0};
    Bytes appInfo, sortInfo;
    std::vector<PalmRecord> records;  // records or resources, by the dbResource flag

    bool Resource() const { return attributes & dbResource; }
    // The file form. Fails only on a name over 31 bytes or more than 65535 entries.
    bool Write(Bytes& out, std::string* error = nullptr) const;
    // Reads a PDB or PRC file. Old files with Unix-epoch times are converted.
    static bool Read(const uint8_t* data, size_t size, PalmDb& db, std::string& error);
    static bool Read(const Bytes& data, PalmDb& db, std::string& error) { return Read(data.data(), data.size(), db, error); }
};

// 'appl' style four-character codes.
std::string FourCc(uint32_t code);
uint32_t FourCc(const char* text);

// Seconds since 1904 from a broken-down calendar time and back, with no time zone.
uint32_t PalmTime(int year, int month, int day, int hour, int minute, int second);
void PalmTimeParts(uint32_t seconds, int& year, int& month, int& day, int& hour, int& minute, int& second);
// Seconds between 1904-01-01 and 1970-01-01.
constexpr uint32_t palmEpochOffset = 2082844800u;

}  // namespace frondSync

#endif
