#ifndef FROND_GUEST_FILES_H
#define FROND_GUEST_FILES_H

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "GuestDatabaseOps.h"

namespace GuestFiles {
using Cancel = std::function<bool()>;
class Cancelled : public std::runtime_error {
 public: Cancelled() : std::runtime_error("File transfer cancelled.") {}
};
inline void CheckCancel(const Cancel& cancel) { if (cancel && cancel()) throw Cancelled(); }
constexpr size_t maxFileBytes = 8 * 1024 * 1024;
constexpr size_t maxEntries = 512;
inline void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
inline void Check(uint16_t error, const char* operation) {
    if (!error) return;
    char message[120]; std::snprintf(message, sizeof(message), "%s failed (Palm OS error %04x).", operation, error);
    throw std::runtime_error(message);
}
inline std::string Name(const std::string& value) {
    Require(!value.empty() && value.size() <= 255 && value != "." && value != "..", "Use a filename of 1-255 characters.");
    Require(value.front() != ' ' && value.back() != ' ' && value.back() != '.', "Filenames cannot start/end with a space or end with a dot.");
    for (unsigned char c : value)
        Require(c >= 32 && c < 127 && std::strchr("/\\:*?\"<>|", c) == nullptr, "Memory Stick filenames currently support printable ASCII without reserved characters.");
    return value;
}
inline std::string Path(const std::string& value, bool root = true) {
    Require(!value.empty() && value.front() == '/' && value.size() <= 511, "Use an absolute card path of at most 511 bytes.");
    if (value == "/") { Require(root, "Choose a file or folder below the card root."); return value; }
    size_t start = 1;
    while (start < value.size()) {
        const auto end = value.find('/', start);
        Name(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) return value;
        start = end + 1;
    }
    throw std::runtime_error("Card paths cannot end with a slash.");
}
inline std::string Join(const std::string& folder, const std::string& name) { return Path((folder == "/" ? folder : folder + '/') + Name(name), false); }
struct Entry { std::string name; bool directory; uint32_t size; };
struct Listing { uint32_t used = 0, total = 0; std::vector<Entry> entries; };

class Session {
 public:
    explicit Session(SoC& soc, Cancel cancel = {}) : sd(soc.GetSyscallDispatch()), cancel(std::move(cancel)) {
        CheckCancel(this->cancel);
        if (std::getenv("FROND_VFS_TRACE")) std::fprintf(stderr, "VFS allocate %u begin\n", unsigned(scratchBytes));
        scratch = syscall68k_MemPtrNew(sd, SC_EXECUTE_PURE, scratchBytes);
        if (std::getenv("FROND_VFS_TRACE")) std::fprintf(stderr, "VFS allocate result %08x\n", unsigned(scratch));
        Require(scratch != 0, "Palm OS could not allocate file-transfer memory.");
        std::vector<uint8_t> zero(scratchBytes); Write(0, zero.data(), zero.size());
        volume = FindVolume();
    }
    void Finish() {
        CheckCancel(cancel);
        Require(files.empty(), "A Palm file reference is still open.");
        Check(syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch), "Free file-transfer memory");
        scratch = 0;
    }
    Listing List(const std::string& path) {
        Listing result;
        Check(syscall68k_VFSVolumeSize(sd, SC_EXECUTE_FULL, volume, scratch + usedAt, scratch + totalAt), "Read card space");
        result.used = Word(usedAt); result.total = Word(totalAt);
        Require(result.used <= result.total, "Palm OS reported invalid card space.");
        const auto directory = Open(path, 2);
        SetWord(iteratorAt, 0);
        for (size_t count = 0; ; ++count) {
            Require(count <= maxEntries, "This folder has more than 512 entries.");
            CheckCancel(cancel);
            const uint32_t prior = Word(iteratorAt);
            if (prior == 0xffffffffu) break;
            std::array<uint8_t, 10> info{};
            Be32(info.data() + 4, scratch + nameAt); Be16(info.data() + 8, 256);
            Write(infoAt, info.data(), info.size());
            std::array<uint8_t, 256> empty{}; Write(nameAt, empty.data(), empty.size());
            const auto error = syscall68k_VFSDirEntryEnumerate(sd, SC_EXECUTE_FULL, directory, scratch + iteratorAt, scratch + infoAt);
            if (error == 0x290d) break;
            Check(error, "Read card folder");
            Require(Word(iteratorAt) != prior, "Palm directory enumeration did not advance.");
            std::array<uint8_t, 256> name{}; Read(nameAt, name.data(), name.size());
            const auto end = std::find(name.begin(), name.end(), uint8_t(0));
            Require(end != name.end(), "Palm OS returned an unterminated filename.");
            const std::string text(name.begin(), end);
            if (text == "." || text == "..") continue;
            if (Word(infoAt) & 0x08) continue;
            Name(text);
            const bool isDirectory = (Word(infoAt) & 0x10) != 0;
            uint32_t size = 0;
            if (!isDirectory) { const auto file = Open(Join(path, text), 2); size = Size(file); Close(file); }
            result.entries.push_back({text, isDirectory, size});
        }
        Close(directory);
        std::sort(result.entries.begin(), result.entries.end(), [](const Entry& a, const Entry& b) {
            return a.directory != b.directory ? a.directory > b.directory : a.name < b.name;
        });
        return result;
    }
    std::vector<uint8_t> Get(const std::string& path) { return ReadContents(path, true); }
    void Put(const std::string& path, const std::vector<uint8_t>& bytes) {
        Require(bytes.size() <= maxFileBytes, "File exchange supports files up to 8 MB.");
        syscallDispatchBeginFileTransfer(sd, uint32_t(bytes.size() * 2));
        Missing(path);
        const auto slash = path.rfind('/'); const auto folder = slash == 0 ? "/" : path.substr(0, slash);
        std::string temporary;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            char name[20]; std::snprintf(name, sizeof(name), "FROND%03X.TMP", attempt);
            const auto candidate = Join(folder, name);
            if (candidate == path) continue;
            if (!Exists(candidate)) { temporary = candidate; break; }
        }
        Require(!temporary.empty(), "No temporary filename is available in this folder.");
        const auto file = Open(temporary, 0x0d);
        for (size_t at = 0; at < bytes.size(); ) {
            CheckCancel(cancel);
            const auto count = std::min(chunkBytes, bytes.size() - at);
            Write(dataAt, bytes.data() + at, count); SetWord(countAt, 0);
            Check(syscall68k_VFSFileWrite(sd, SC_EXECUTE_FULL, file, count, scratch + dataAt, scratch + countAt), "Write card file");
            Require(Word(countAt) == count, "Palm OS wrote fewer bytes than requested.");
            syscallDispatchFileProgress(sd, uint32_t(count));
            at += count;
        }
        Close(file);
        const auto verify = ReadContents(temporary, false);
        Require(verify == bytes, "The copy on the Memory Stick does not match the source.");
        Missing(path);
        Rename(temporary, path.substr(slash + 1));
    }
    void Mkdir(const std::string& path) {
        Missing(path); String(pathAt, path);
        Check(syscall68k_VFSDirCreate(sd, SC_EXECUTE_FULL, volume, scratch + pathAt), "Create card folder");
    }
    void Delete(const std::string& path) {
        String(pathAt, path);
        Check(syscall68k_VFSFileDelete(sd, SC_EXECUTE_FULL, volume, scratch + pathAt), "Delete card entry");
    }
    void Rename(const std::string& path, const std::string& name) {
        Name(name);
        const auto slash = path.rfind('/'); const auto folder = slash == 0 ? "/" : path.substr(0, slash);
        const auto destination = Join(folder, name);
        Require(path != destination, "Choose a different filename."); Missing(destination);
        String(pathAt, path); String(otherPathAt, name);
        Check(syscall68k_VFSFileRename(sd, SC_EXECUTE_FULL, volume, scratch + pathAt, scratch + otherPathAt), "Rename card entry");
    }
 private:
    static constexpr size_t chunkBytes = 32768;
    static constexpr uint32_t dataAt = 2048, scratchBytes = dataAt + chunkBytes;
    static constexpr uint32_t pathAt = 0, otherPathAt = 512, nameAt = 1024, infoAt = 1280;
    static constexpr uint32_t iteratorAt = 1320, volumeAt = 1324, countAt = 1328, usedAt = 1332, totalAt = 1336;
    SyscallDispatch* sd;
    Cancel cancel;
    uint32_t scratch = 0; uint16_t volume = 0; std::set<uint32_t> files;
    static void Be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
    static void Be32(uint8_t* p, uint32_t v) { Be16(p, v >> 16); Be16(p + 2, v); }
    void Write(uint32_t offset, const void* data, size_t size) {
        Require(offset <= scratchBytes && size <= scratchBytes - offset && syscallDispatch_memcpy_fromHost(sd, scratch + offset, data, size), "Could not write Palm file-transfer memory.");
    }
    void Read(uint32_t offset, void* data, size_t size) {
        Require(offset <= scratchBytes && size <= scratchBytes - offset && syscallDispatch_memcpy_toHost(sd, data, scratch + offset, size), "Could not read Palm file-transfer memory.");
    }
    void SetWord(uint32_t offset, uint32_t value) { uint8_t bytes[4]; Be32(bytes, value); Write(offset, bytes, 4); }
    uint32_t Word(uint32_t offset) { uint8_t bytes[4]; Read(offset, bytes, 4); return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3]; }
    void String(uint32_t offset, const std::string& value) { Require(value.size() < 512, "The card path is too long."); Write(offset, value.c_str(), value.size() + 1); }
    uint16_t FindVolume() {
        SetWord(iteratorAt, 0); uint16_t found = 0;
        for (unsigned count = 0; count < 32; ++count) {
            const auto prior = Word(iteratorAt); if (prior == 0xffffffffu) { Require(found != 0, "No mounted Memory Stick volume was found."); return found; }
            Check(syscall68k_VFSVolumeEnumerate(sd, SC_EXECUTE_FULL, scratch + volumeAt, scratch + iteratorAt), "Find Memory Stick volume");
            Require(Word(iteratorAt) != prior, "Palm volume enumeration did not advance.");
            uint8_t v[2]; Read(volumeAt, v, 2); const uint16_t current = uint16_t(v[0]) << 8 | v[1];
            Require(current != 0, "Palm OS returned an invalid volume reference.");
            if (std::getenv("FROND_VFS_TRACE")) std::fprintf(stderr, "VFS volume %u iterator %08x info %08x\n", unsigned(current), unsigned(Word(iteratorAt)), unsigned(scratch + infoAt));
            Check(syscall68k_VFSVolumeInfo(sd, SC_EXECUTE_FULL, current, scratch + infoAt), "Identify card volume");
            if (Word(infoAt + 12) != 0x6c696273 || Word(infoAt + 20) != 0x6d73746b) continue;
            uint8_t slot[4]; Read(infoAt + 16, slot, 4); Require(slot[0] || slot[1], "The Memory Stick volume has no slot driver.");
            if (std::getenv("FROND_VFS_TRACE")) std::fprintf(stderr, "VFS Memory Stick volume=%u slotLibrary=%u slot=%u\n", unsigned(current), unsigned(uint16_t(slot[0]) << 8 | slot[1]), unsigned(uint16_t(slot[2]) << 8 | slot[3]));
            Require(found == 0, "More than one Memory Stick volume is mounted."); found = current;
        }
        if (Word(iteratorAt) == 0xffffffffu && found) return found;
        throw std::runtime_error("Too many mounted Palm volumes.");
    }
    uint32_t Open(const std::string& path, uint16_t mode) {
        String(pathAt, path); SetWord(countAt, 0);
        Check(syscall68k_VFSFileOpen(sd, SC_EXECUTE_FULL, volume, scratch + pathAt, mode, scratch + countAt), "Open card entry");
        const auto file = Word(countAt); Require(file && !files.count(file), "Palm OS returned an invalid file reference."); files.insert(file); return file;
    }
    void Close(uint32_t file) { Require(files.count(file), "Unknown Palm file reference."); Check(syscall68k_VFSFileClose(sd, SC_EXECUTE_FULL, file), "Close card entry"); files.erase(file); }
    uint32_t Size(uint32_t file) { Check(syscall68k_VFSFileSize(sd, SC_EXECUTE_FULL, file, scratch + countAt), "Read card file size"); return Word(countAt); }
    uint32_t Attributes(uint32_t file) { Check(syscall68k_VFSFileGetAttributes(sd, SC_EXECUTE_FULL, file, scratch + countAt), "Read card attributes"); return Word(countAt); }
    bool Exists(const std::string& path) {
        String(pathAt, path); SetWord(countAt, 0);
        const auto error = syscall68k_VFSFileOpen(sd, SC_EXECUTE_FULL, volume, scratch + pathAt, 2, scratch + countAt);
        if (error == 0x2a08) return false;
        Check(error, "Check card filename");
        const auto file = Word(countAt); Require(file && !files.count(file), "Palm OS returned an invalid file reference."); files.insert(file); Close(file); return true;
    }
    void Missing(const std::string& path) { Require(!Exists(path), "That name already exists on the Memory Stick."); }
    std::vector<uint8_t> ReadContents(const std::string& path, bool ownPlan) {
        const auto file = Open(path, 2);
        Require(!(Attributes(file) & 0x10), "Choose a regular file.");
        const uint32_t size = Size(file);
        Require(size <= maxFileBytes, "File exchange supports files up to 8 MB.");
        if (ownPlan) syscallDispatchBeginFileTransfer(sd, size);
        std::vector<uint8_t> result(size);
        ReadFile(file, result);
        Require(Size(file) == size, "The Palm file changed size during the transfer.");
        Close(file);
        return result;
    }
    void ReadFile(uint32_t file, std::vector<uint8_t>& bytes) {
        for (size_t at = 0; at < bytes.size(); ) {
            CheckCancel(cancel);
            const auto count = std::min(chunkBytes, bytes.size() - at); SetWord(countAt, 0);
            const auto error = syscall68k_VFSFileRead(sd, SC_EXECUTE_FULL, file, count, scratch + dataAt, scratch + countAt);
            if (error != 0x2a07) Check(error, "Read card file");
            Require(Word(countAt) == count, "Palm OS returned fewer bytes than requested.");
            Read(dataAt, bytes.data() + at, count);
            syscallDispatchFileProgress(sd, uint32_t(count));
            at += count;
        }
    }
};
inline Listing List(SoC& soc, const std::string& path, const Cancel& cancel = {}) {
    Path(path); CheckCancel(cancel); ReadOnlyGuest transaction(soc); Session files(soc, cancel);
    auto result = files.List(path); files.Finish(); CheckCancel(cancel); syscallDispatchValidateCommit(soc.GetSyscallDispatch()); return result;
}
inline std::vector<uint8_t> Get(SoC& soc, const std::string& path, const Cancel& cancel = {}) {
    Path(path, false); CheckCancel(cancel); ReadOnlyGuest transaction(soc); Session files(soc, cancel);
    auto result = files.Get(path); files.Finish(); CheckCancel(cancel); syscallDispatchValidateCommit(soc.GetSyscallDispatch()); return result;
}
inline void Put(SoC& soc, const std::string& path, const std::vector<uint8_t>& bytes, const Cancel& cancel = {}) {
    Path(path, false); CheckCancel(cancel); Require(bytes.size() <= maxFileBytes, "File exchange supports files up to 8 MB.");
    ReadOnlyGuest transaction(soc); Session files(soc, cancel); files.Put(path, bytes); files.Finish(); CheckCancel(cancel); transaction.Commit();
}
inline void Mkdir(SoC& soc, const std::string& path, const Cancel& cancel = {}) { Path(path, false); CheckCancel(cancel); ReadOnlyGuest transaction(soc); Session files(soc, cancel); files.Mkdir(path); files.Finish(); CheckCancel(cancel); transaction.Commit(); }
inline void Delete(SoC& soc, const std::string& path, const Cancel& cancel = {}) { Path(path, false); CheckCancel(cancel); ReadOnlyGuest transaction(soc); Session files(soc, cancel); files.Delete(path); files.Finish(); CheckCancel(cancel); transaction.Commit(); }
inline void Rename(SoC& soc, const std::string& path, const std::string& name, const Cancel& cancel = {}) { Path(path, false); Name(name); CheckCancel(cancel); ReadOnlyGuest transaction(soc); Session files(soc, cancel); files.Rename(path, name); files.Finish(); CheckCancel(cancel); transaction.Commit(); }
}
#endif
