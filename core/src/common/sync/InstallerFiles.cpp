#include "InstallerFiles.h"

#include <algorithm>
#include <cctype>

#include "zip/miniz.h"

namespace frondSync {
namespace {

constexpr size_t maxFile = 64 * 1024 * 1024;
constexpr size_t maxTotal = 128 * 1024 * 1024;
constexpr size_t maxFiles = 64;
constexpr size_t maxNestedFiles = 8;
constexpr unsigned maxDepth = 2;
// Include invalid and duplicate entries, plus the wrappers holding valid payloads.
constexpr size_t maxInflatedBytes = 2 * maxTotal;
constexpr size_t maxInflatedFiles = 2 * maxFiles;
constexpr auto limitError = "The installer has too many files or exceeds the unpacked size limit.";

std::string Extension(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return {};
    auto extension = name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
    return extension;
}

uint32_t Little32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool IsCabinet(const uint8_t* p, size_t remaining) {
    if (remaining < 36 || !std::equal(p, p + 4, "MSCF")) return false;
    const auto size = Little32(p + 8);
    const auto fileTable = Little32(p + 16);
    return size >= 36 && size <= maxFile && size <= remaining && fileTable >= 36 && fileTable < size &&
           !Little32(p + 4) && !Little32(p + 12) && !Little32(p + 20) && p[24] == 3 && p[25] == 1;
}

struct ZipReader {
    mz_zip_archive zip{};
    ~ZipReader() { if (zip.m_pState) mz_zip_reader_end(&zip); }
};

bool IsZipOrSetup(const std::string& extension) {
    return extension == ".zip" || extension == ".exe" || extension == ".msi";
}

struct UnpackedFiles {
    std::vector<InstallerFile>& files;
    std::string& error;
    const std::function<bool()>& stopped;
    size_t total = 0;
    size_t inflatedBytes = 0;
    size_t inflatedFiles = 0;
    size_t nestedCount = 0;
    size_t nestedBytes = 0;

    bool Store(InstallerFile&& file) {
        if (!IsInstallPayload(file)) return true;
        for (const auto& existing : files) {
            if (existing.name == file.name && existing.bytes == file.bytes) return true;
        }
        if (files.size() >= maxFiles || file.bytes.size() > maxTotal - total) {
            error = limitError;
            return false;
        }
        total += file.bytes.size();
        files.push_back(std::move(file));
        return true;
    }

    bool Stopped() {
        if (!stopped || !stopped()) return false;
        error = "Extraction was cancelled.";
        return true;
    }

    bool ReserveInflation(size_t size) {
        if (inflatedFiles >= maxInflatedFiles || size > maxInflatedBytes - inflatedBytes) {
            error = limitError;
            return false;
        }
        ++inflatedFiles;
        inflatedBytes += size;
        return true;
    }

    bool CanOpenNested(size_t size) {
        if (nestedCount >= maxNestedFiles || size > maxTotal - nestedBytes) return false;
        ++nestedCount;
        nestedBytes += size;
        return true;
    }
};

bool Unpack(const std::string& name, const Bytes& bytes, UnpackedFiles& found, unsigned depth) {
    if (found.Stopped()) return false;
    const auto extension = Extension(name);
    const auto originalCount = found.files.size();
    ZipReader reader;
    if (mz_zip_reader_init_mem(&reader.zip, bytes.data(), bytes.size(), 0)) {
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader.zip); ++i) {
            if (found.Stopped()) return false;
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&reader.zip, i, &stat) || mz_zip_reader_is_file_a_directory(&reader.zip, i)) continue;
            const std::string path(stat.m_filename);
            InstallerFile file{path.substr(path.find_last_of("/\\") + 1), {}};
            if (file.name.size() > 255 || !stat.m_uncomp_size || stat.m_uncomp_size > maxFile) continue;
            const bool direct = IsInstallFileName(file.name);
            if (!direct && (depth >= maxDepth || !IsZipOrSetup(Extension(file.name)) ||
                            !found.CanOpenNested(stat.m_uncomp_size))) continue;
            if (!found.ReserveInflation(stat.m_uncomp_size)) return false;
            file.bytes.resize(stat.m_uncomp_size);
            if (!mz_zip_reader_extract_to_mem(&reader.zip, i, file.bytes.data(), file.bytes.size(), 0)) continue;
            const bool ok = direct ? found.Store(std::move(file)) : Unpack(file.name, file.bytes, found, depth + 1);
            if (!ok) return false;
        }
    }
    if (found.files.size() != originalCount || (extension != ".exe" && extension != ".msi")) return true;

    const std::string stem = name.substr(0, name.rfind('.'));
    size_t index = 0;
    for (size_t offset = 0; offset + 36 <= bytes.size();) {
        if (!(offset & 0xffff) && found.Stopped()) return false;
        const auto* key = bytes.data() + offset;
        if (!IsCabinet(key, bytes.size() - offset)) {
            ++offset;
            continue;
        }
        if (found.Stopped()) return false;
        const auto length = Little32(key + 8);
        InstallerFile file{stem.substr(0, 230) + "-" + std::to_string(++index) + ".cab", Bytes(key, key + length)};
        if (!found.Store(std::move(file))) return false;
        offset += length;
    }
    return true;
}

}  // namespace

bool IsInstallPayload(const InstallerFile& file) {
    if (Extension(file.name) == ".cab") return IsCabinet(file.bytes.data(), file.bytes.size());
    PalmDb db;
    std::string ignored;
    return PalmDb::Read(file.bytes, db, ignored);
}

bool IsInstallFileName(const std::string& name) {
    const auto extension = Extension(name);
    return extension == ".prc" || extension == ".pdb" || extension == ".pqa" || extension == ".cab";
}

bool IsInstallerFileName(const std::string& name) {
    const auto extension = Extension(name);
    return IsZipOrSetup(extension) || extension == ".7z" || extension == ".rar";
}

bool UnpackInstaller(const std::string& name, const Bytes& bytes, std::vector<InstallerFile>& files, std::string& error,
                     bool openNested, const std::function<bool()>& stopped) {
    files.clear();
    std::vector<InstallerFile> found;
    UnpackedFiles unpacked{found, error, stopped};
    if (!Unpack(name, bytes, unpacked, openNested ? 0 : maxDepth) || unpacked.Stopped()) return false;
    if (found.empty()) {
        error = "No Palm or Windows Mobile install files were found.";
        return false;
    }
    files = std::move(found);
    return true;
}

}  // namespace frondSync
