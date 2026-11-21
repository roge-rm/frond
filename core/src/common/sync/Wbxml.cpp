#include "Wbxml.h"

#include <cstdint>
#include <cstdio>

namespace frondSync {
namespace {

constexpr uint8_t switchPage = 0x00, end = 0x01, strI = 0x03, strT = 0x83, opaque = 0xc3;
constexpr uint8_t contentBit = 0x40, attributeBit = 0x80;

void PutMultiByte(std::vector<uint8_t>& out, uint32_t value) {
    uint8_t bytes[5];
    int count = 0;
    do {
        bytes[count++] = uint8_t(value & 0x7f);
        value >>= 7;
    } while (value);
    while (count--) out.push_back(uint8_t(bytes[count] | (count ? 0x80 : 0)));
}

void EncodeNode(const WbxmlNode& node, uint8_t& page, std::vector<uint8_t>& out) {
    if (node.page != page) {
        out.push_back(switchPage);
        out.push_back(node.page);
        page = node.page;
    }
    const bool content = !node.text.empty() || node.opaque || !node.children.empty();
    out.push_back(uint8_t(node.tag | (content ? contentBit : 0)));
    if (!content) return;
    if (node.opaque) {
        out.push_back(opaque);
        PutMultiByte(out, uint32_t(node.text.size()));
        out.insert(out.end(), node.text.begin(), node.text.end());
    } else if (!node.text.empty()) {
        out.push_back(strI);
        out.insert(out.end(), node.text.begin(), node.text.end());
        out.push_back(0);
    }
    for (const auto& child : node.children) EncodeNode(child, page, out);
    out.push_back(end);
}

struct Decoder {
    const uint8_t* data;
    size_t size, pos{0};
    std::string table;
    uint8_t page{0};

    bool Byte(uint8_t& value) {
        if (pos >= size) return false;
        value = data[pos++];
        return true;
    }
    bool MultiByte(uint32_t& value) {
        value = 0;
        for (int i = 0; i < 5; ++i) {
            uint8_t b;
            if (!Byte(b) || value > (UINT32_MAX >> 7)) return false;  // past 32 bits
            value = value << 7 | (b & 0x7f);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool Element(WbxmlNode& node, uint8_t token, int depth) {
        if (token & attributeBit || depth > 64) return false;
        node.page = page;
        node.tag = token & 0x3f;
        if (!(token & contentBit)) return true;
        for (;;) {
            uint8_t b;
            if (!Byte(b)) return false;
            if (b == end) return true;
            if (b == switchPage) {
                if (!Byte(page)) return false;
            } else if (b == strI) {
                const size_t start = pos;
                while (pos < size && data[pos]) ++pos;
                if (pos >= size) return false;
                node.text.append(reinterpret_cast<const char*>(data + start), pos - start);
                ++pos;
            } else if (b == strT) {
                uint32_t offset;
                if (!MultiByte(offset) || offset >= table.size()) return false;
                node.text += table.c_str() + offset;
            } else if (b == opaque) {
                uint32_t length;
                if (!MultiByte(length) || length > size - pos) return false;
                node.text.append(reinterpret_cast<const char*>(data + pos), length);
                node.opaque = true;
                pos += length;
            } else if ((b & 0x3f) >= 5) {
                if (!Element(node.children.emplace_back(), b, depth + 1)) return false;
            } else {
                return false;
            }
        }
    }
};

}  // namespace

const WbxmlNode* WbxmlNode::Find(uint8_t childPage, uint8_t childTag) const {
    for (const auto& child : children)
        if (child.page == childPage && child.tag == childTag) return &child;
    return nullptr;
}

std::string WbxmlNode::Text(uint8_t childPage, uint8_t childTag, const std::string& fallback) const {
    const auto* child = Find(childPage, childTag);
    return child ? child->text : fallback;
}

std::vector<uint8_t> EncodeWbxml(const WbxmlNode& root) {
    // Version 1.3, unknown public identifier, UTF-8, empty string table.
    std::vector<uint8_t> out{0x03, 0x01, 0x6a, 0x00};
    uint8_t page = 0;
    EncodeNode(root, page, out);
    return out;
}

bool DecodeWbxml(const uint8_t* data, size_t size, WbxmlNode& root, std::string& error) {
    Decoder in{data, size};
    uint8_t version, token;
    uint32_t publicId, charset, tableSize;
    if (!in.Byte(version) || !in.MultiByte(publicId) || (publicId == 0 && !in.MultiByte(publicId)) ||
        !in.MultiByte(charset) || !in.MultiByte(tableSize) || tableSize > size - in.pos) {
        error = "Truncated WBXML header";
        return false;
    }
    if (charset != 106) {
        error = "WBXML is not UTF-8";
        return false;
    }
    in.table.assign(reinterpret_cast<const char*>(data + in.pos), tableSize);
    in.pos += tableSize;
    // The root element, after any page switches; a body that ends first is malformed.
    bool haveRoot = in.Byte(token);
    while (haveRoot && token == switchPage) haveRoot = in.Byte(in.page) && in.Byte(token);
    root = {};
    if (!haveRoot || !in.Element(root, token, 0)) {
        error = "Malformed WBXML";
        return false;
    }
    return true;
}

std::string DumpWbxml(const WbxmlNode& node, int depth) {
    char name[16];
    std::snprintf(name, sizeof(name), "%u:%02x", node.page, node.tag);
    std::string out(size_t(depth) * 2, ' ');
    out += name;
    if (node.opaque) out += " [" + std::to_string(node.text.size()) + " bytes]";
    else if (!node.text.empty()) out += " \"" + node.text + "\"";
    out += "\n";
    for (const auto& child : node.children) out += DumpWbxml(child, depth + 1);
    return out;
}

}  // namespace frondSync
