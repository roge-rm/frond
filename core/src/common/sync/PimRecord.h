#ifndef FROND_SYNC_PIM_RECORD_H
#define FROND_SYNC_PIM_RECORD_H

#include <map>
#include <string>

#include "PalmDb.h"

namespace frondSync {

enum class PimKind { contacts, calendar, tasks, memos };
enum class PimAction { synchronize, desktopOverwrites, handheldOverwrites, skip };
enum class PalmPimFormat { classic, garnet };

struct PimRecord {
    int64_t id{0};
    PimKind kind{PimKind::memos};
    int64_t revision{0};  // Required when updating an existing record.
    std::string category;
    bool secret{false}, archived{false}, deleted{false};

    // UTF-8 text, dates as YYYY-MM-DD and local times as HH:MM. Keys are shared by both
    // Palm formats. An absent field differs from a present empty field.
    std::map<std::string, std::string> fields;
    std::map<std::string, Bytes> extras;  // Format-specific fields with no common representation.
};

const char* PimKindName(PimKind kind);
const char* PimActionName(PimAction action);
bool ParsePimKind(const std::string& name, PimKind& kind);
bool ParsePimAction(const std::string& name, PimAction& action);

std::string PalmPimText(const Bytes& raw);
bool EncodePalmPimText(const std::string& utf8, Bytes& out);
bool DecodePalmPim(PimKind kind, PalmPimFormat format, const Bytes& data, PimRecord& out, std::string& error);
bool EncodePalmPim(const PimRecord& record, PalmPimFormat format, Bytes& out, std::string& error);

// Only fields represented by this wire format are replaced.
void ApplyPalmPim(const PimRecord& decoded, PalmPimFormat format, PimRecord& record);

}  // namespace frondSync

#endif
