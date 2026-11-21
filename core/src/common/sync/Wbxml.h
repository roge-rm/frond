#ifndef FROND_SYNC_WBXML_H
#define FROND_SYNC_WBXML_H

// WBXML 1.3 as Exchange ActiveSync uses it (MS-ASWBXML): code pages of tags, inline strings
// and opaque data, no attributes and no string table.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace frondSync {

struct WbxmlNode {
    uint8_t page{0}, tag{0};  // tag is the token without the content bit
    std::string text;         // inline string or opaque bytes
    bool opaque{false};
    std::vector<WbxmlNode> children;

    WbxmlNode() = default;
    WbxmlNode(uint8_t page, uint8_t tag) : page(page), tag(tag) {}
    WbxmlNode(uint8_t page, uint8_t tag, std::string text) : page(page), tag(tag), text(std::move(text)) {}

    WbxmlNode& Add(uint8_t childPage, uint8_t childTag) { return children.emplace_back(childPage, childTag); }
    WbxmlNode& Add(uint8_t childPage, uint8_t childTag, std::string value) {
        return children.emplace_back(childPage, childTag, std::move(value));
    }
    // The first child with this tag, or nullptr.
    const WbxmlNode* Find(uint8_t childPage, uint8_t childTag) const;
    // The first such child's text, or fallback when there is none.
    std::string Text(uint8_t childPage, uint8_t childTag, const std::string& fallback = {}) const;
};

std::vector<uint8_t> EncodeWbxml(const WbxmlNode& root);
// False on malformed input or anything outside the subset above.
bool DecodeWbxml(const uint8_t* data, size_t size, WbxmlNode& root, std::string& error);
// An indented text form with page:tag names, for logs and tests.
std::string DumpWbxml(const WbxmlNode& node, int depth = 0);

}  // namespace frondSync

#endif
