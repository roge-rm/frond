#ifndef FROND_DB_ARCHIVE_NAME_H
#define FROND_DB_ARCHIVE_NAME_H
#include <cstdint>
#include <stdexcept>
#include <string>

// A Palm database name is a single logical name, not an archive path. Escape
// separators and '%' reversibly so valid guest names cannot traverse directories
// or collide with their escaped spelling. The PDB/PRC header remains unchanged.
inline std::string dbArchiveName(const std::string& name, uint32_t attributes,
                                 uint32_t creator, bool isRom) {
    if (name.empty() || name.size() > 31 || name.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid exported database name");
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char c : name) {
        if (c == '/' || c == '\\' || c == '%' || c == ':' || c < 32 || c == 127) {
            encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15];
        } else encoded += char(c);
    }
    const char* suffix = creator == 0x636c7072 && (attributes & 0x0200) ? ".pqa" :
                         (attributes & 1) ? ".prc" : ".pdb";
    return (isRom ? "rom/" : creator == 0x6136386b ? "cache/" : "") + encoded + suffix;
}
#endif
