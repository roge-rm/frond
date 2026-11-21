#include "InstallerFiles.h"

#include <algorithm>
#include <cctype>

#include "zip/miniz.h"

namespace frondSync {
namespace {

constexpr size_t maxFile = 64 * 1024 * 1024;
constexpr size_t maxTotal = 128 * 1024 * 1024;
constexpr size_t maxFiles = 64;

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

bool Store(std::vector<InstallerFile>& files, size_t& total, InstallerFile&& file, std::string& error) {
    if (!IsInstallPayload(file)) return true;
    if (files.size() >= maxFiles || file.bytes.size() > maxTotal - total) {
        error = "The installer has too many files or exceeds the unpacked size limit.";
        return false;
    }
    total += file.bytes.size();
    files.push_back(std::move(file));
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
    return extension == ".zip" || extension == ".exe" || extension == ".msi";
}

bool UnpackInstaller(const std::string& name, const Bytes& bytes, std::vector<InstallerFile>& files, std::string& error) {
    files.clear();
    std::vector<InstallerFile> found;
    size_t total = 0;
    ZipReader reader;
    if (mz_zip_reader_init_mem(&reader.zip, bytes.data(), bytes.size(), 0)) {
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader.zip); ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&reader.zip, i, &stat) || mz_zip_reader_is_file_a_directory(&reader.zip, i)) continue;
            const std::string path(stat.m_filename);
            InstallerFile file{path.substr(path.find_last_of("/\\") + 1), {}};
            if (!IsInstallFileName(file.name) || file.name.size() > 255 || stat.m_uncomp_size > maxFile) continue;
            file.bytes.resize(stat.m_uncomp_size);
            if (!mz_zip_reader_extract_to_mem(&reader.zip, i, file.bytes.data(), file.bytes.size(), 0)) continue;
            if (!Store(found, total, std::move(file), error)) return false;
        }
    }
    if (found.empty() && Extension(name) != ".zip") {
        const std::string stem = name.substr(0, name.rfind('.'));
        for (size_t offset = 0; offset + 36 <= bytes.size();) {
            const auto* key = bytes.data() + offset;
            if (!IsCabinet(key, bytes.size() - offset)) {
                ++offset;
                continue;
            }
            const auto length = Little32(key + 8);
            InstallerFile file{stem.substr(0, 230) + "-" + std::to_string(found.size() + 1) + ".cab", Bytes(key, key + length)};
            if (!Store(found, total, std::move(file), error)) return false;
            offset += length;
        }
    }
    if (found.empty()) {
        error = "No Palm or Windows Mobile install files were found.";
        return false;
    }
    files = std::move(found);
    return true;
}

}  // namespace frondSync
