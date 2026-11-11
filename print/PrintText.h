#pragma once
// Turns a raw Generic (6.0) PrintBoy job into pages of text lines. Header-only, no Qt, so the
// desktop and Android native code can both include it.
//
// Rules: CR, LF and CRLF end a line (CRLF counts once). Form feed ends the page. Tab goes to the next
// multiple of 8 columns. Lines wrap at 80 columns and pages hold 66 lines; a line that ends exactly at
// the limit does not add a blank line or page. A form feed at the very end adds no empty page.
// Any other byte below 0x20, and 0x7f, means the job is not plain text (renderable is false).
// Bytes 0x80-0xff are Palm's Latin charset, read as Windows-1252 apart from 0x8d-0x90, which Palm's
// fonts (the Dana's included) still draw as the diamond, club, heart and spade suits. 0x81 and 0x9d
// become U+FFFD, and 0xa0 becomes a plain space. Lines are UTF-8.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace frond {

struct PrintText {
    bool renderable = true;
    std::string reason;
    std::vector<std::vector<std::string>> pages;
};

constexpr int kPrintColumns = 80;
constexpr int kPrintLinesPerPage = 66;

inline void printAppendUtf8(std::string &s, uint32_t c) {
    if(c < 0x80) s += char(c);
    else if(c < 0x800) { s += char(0xc0 | (c >> 6)); s += char(0x80 | (c & 0x3f)); }
    else { s += char(0xe0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3f)); s += char(0x80 | (c & 0x3f)); }
}

inline uint32_t printHighByteToUnicode(uint8_t b) {
    static const uint16_t cp1252[32] = {
        0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x2666, 0x2663, 0x2665,
        0x2660, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178};
    if(b < 0xa0) return cp1252[b - 0x80];
    if(b == 0xa0) return 0x20;
    return b;
}

inline PrintText renderPrintText(const uint8_t *data, size_t size) {
    PrintText out;
    std::vector<std::string> page;
    std::string line;   // UTF-8 of the current line
    int col = 0;
    bool lineOpen = false;   // the current line has text or a tab
    auto endLine = [&]() {
        if(int(page.size()) >= kPrintLinesPerPage) { out.pages.push_back(std::move(page)); page.clear(); }
        page.push_back(std::move(line));
        line.clear();
        col = 0;
        lineOpen = false;
    };
    auto endPage = [&]() {
        if(lineOpen) endLine();
        out.pages.push_back(std::move(page));
        page.clear();
    };
    bool pageUsed = false;   // anything since the last form feed
    for(size_t i = 0; i < size; i++) {
        uint8_t b = data[i];
        if(b == '\r' || b == '\n') {
            if(b == '\r' && i + 1 < size && data[i + 1] == '\n') i++;
            endLine();
            pageUsed = true;
        } else if(b == '\f') {
            endPage();
            pageUsed = false;
        } else if(b == '\t') {
            if(col >= kPrintColumns) endLine();
            int next = (col / 8 + 1) * 8;
            line.append(size_t(next - col), ' ');
            col = next;
            lineOpen = pageUsed = true;
        } else if(b < 0x20 || b == 0x7f) {
            PrintText bad;
            bad.renderable = false;
            bad.reason = "Only the Generic printer profile can be shown as pages";
            return bad;
        } else {
            if(col >= kPrintColumns) endLine();
            if(b < 0x7f) line += char(b);
            else printAppendUtf8(line, printHighByteToUnicode(b));
            col++;
            lineOpen = pageUsed = true;
        }
    }
    if(lineOpen) endLine();
    if(pageUsed && !page.empty()) out.pages.push_back(std::move(page));
    return out;
}

inline PrintText renderPrintText(const std::vector<uint8_t> &bytes) { return renderPrintText(bytes.data(), bytes.size()); }

}  // namespace frond
