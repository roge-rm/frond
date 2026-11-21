#include "PwiNote.h"

#include <algorithm>
#include <array>
#include <utility>

namespace frondSync {
namespace {

constexpr size_t textStart = 0x172;
constexpr size_t paragraphStart = 0x14e;
constexpr size_t lineSize = 36;
constexpr uint8_t paragraphEnd = 0xc4;

uint16_t Word(const Bytes& data, size_t offset) {
    return data[offset] | (uint16_t(data[offset + 1]) << 8);
}

// Fixed words in the observed WM5 Notes header and single default-font paragraph.
// All other words must be zero except the fields that vary with the text.
constexpr std::pair<size_t, uint16_t> fixedWords[] = {
    {0x000, 0x5c7b}, {0x002, 0x7770}, {0x004, 0x1569}, {0x008, 0x0101}, {0x00a, 6},
    {0x010, 2}, {0x012, 0x18}, {0x014, 1}, {0x016, 0x0a}, {0x018, 1}, {0x01c, 0x64},
    {0x024, 1}, {0x026, 1},
    {0x034, 'T'}, {0x036, 'a'}, {0x038, 'h'}, {0x03a, 'o'}, {0x03c, 'm'}, {0x03e, 'a'},
    {0x074, 0x14}, {0x076, 4}, {0x078, 1}, {0x07e, 0x15}, {0x080, 5}, {0x082, 1},
    {0x088, 7}, {0x08a, 6}, {0x08c, 0x15}, {0x08e, 0x50}, {0x090, 1},
    {0x092, 0x2fd0}, {0x096, 0x3de0}, {0x09a, 0xf0},
    {0x09e, 0x5a0}, {0x0a2, 0x5a0}, {0x0a6, 0x5a0}, {0x0aa, 0x5a0},
    {0x0bc, 0x0a}, {0x0c2, 4}, {0x0cc, 0x0a}, {0x0d2, 4},
    {0x0e2, 8}, {0x0e4, 7}, {0x0e6, 0x10}, {0x0e8, 1}, {0x0ec, 2},
    {0x0fc, 0x12}, {0x104, 0x3e4}, {0x108, 0x353}, {0x114, 0x402}, {0x118, 0x353},
    {0x128, 0x40}, {0x12a, 8}, {0x12c, 8}, {0x12e, 1}, {0x132, 1}, {0x13a, 1}, {0x146, 1},
    {paragraphStart, 0x41}, {0x158, 1}, {0x16c, 0x01e5}, {0x16e, 0xe600}, {0x170, 0x0a},
};

bool PlainAscii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; });
}

template <typename Buffer>
void SetWord(Buffer& buffer, size_t offset, uint16_t value) {
    buffer[offset] = value & 0xff;
    buffer[offset + 1] = value >> 8;
}

std::array<uint8_t, textStart> Header(uint16_t count, uint16_t setting, uint16_t layout,
                                      uint16_t paragraphWords, uint16_t lineLayout) {
    std::array<uint8_t, textStart> header{};
    for (const auto& [offset, value] : fixedWords) SetWord(header, offset, value);
    SetWord(header, 0x01a, setting);
    for (size_t offset : {0x136, 0x148, 0x154}) SetWord(header, offset, count);
    SetWord(header, 0x156, count + 7);
    SetWord(header, 0x14a, layout);
    SetWord(header, 0x150, layout);
    SetWord(header, 0x152, paragraphWords);
    SetWord(header, 0x15c, lineLayout);
    return header;
}

bool MatchesHeader(const Bytes& data, uint16_t count) {
    const auto setting = Word(data, 0x01a);
    if (setting != 100 && setting != 200) return false;
    const auto header = Header(count, setting, Word(data, 0x150), Word(data, 0x152), Word(data, 0x15c));
    return std::equal(header.begin(), header.end(), data.begin());
}

std::array<uint8_t, lineSize> Line(uint16_t count, uint16_t layout, uint16_t width) {
    return {
        0x42, 0, uint8_t(layout), uint8_t(layout >> 8), 3, 0, uint8_t(count), uint8_t(count >> 8),
        0, 0, 0, 0, uint8_t(width), uint8_t(width >> 8), 0x0d, 3, 0, 0,
        0x82, 0, 9, 0, 3, 0, 0x82, 0,
    };
}

bool MatchesLineAndEnd(const Bytes& data, size_t line, uint16_t count) {
    const auto expected = Line(count, Word(data, 0x15c), Word(data, line + 12));
    return std::equal(expected.begin(), expected.end(), data.begin() + line);
}

}  // namespace

std::optional<std::string> ReadPwiNote(const Bytes& data) {
    if (data.size() < textStart + lineSize) return std::nullopt;
    const uint16_t count = Word(data, 0x136);
    const size_t line = paragraphStart + 6 + 4 * size_t(Word(data, 0x152));
    if (count < 2 || count > UINT16_MAX - 7 || line + lineSize != data.size() || line < textStart + count ||
        line - (textStart + count) > 7 || !MatchesHeader(data, count))
        return std::nullopt;

    const size_t end = textStart + count - 1;
    if (data[end] != paragraphEnd ||
        !std::all_of(data.begin() + end + 1, data.begin() + line, [](uint8_t byte) { return byte == 0; }) ||
        !MatchesLineAndEnd(data, line, count))
        return std::nullopt;
    std::string text(data.begin() + textStart, data.begin() + end);
    if (!PlainAscii(text)) return std::nullopt;
    return text;
}

std::optional<Bytes> WritePwiNote(std::string_view text) {
    // Keep generated text short enough for the default Notes layout.
    if (text.empty() || text.size() > 21 || !PlainAscii(text)) return std::nullopt;

    const auto count = uint16_t(text.size() + 1);
    // Use the padding pattern from the 424- and 432-byte typed samples.
    const size_t line = textStart + 2 + ((count + 5) & ~size_t(7));
    const auto paragraphWords = uint16_t((line - paragraphStart - 6) / 4);
    // Use the default single-line layout and an estimated width. Notes recalculates it on save.
    const auto header = Header(count, 200, 35, paragraphWords, 34);
    const auto trailer = Line(count, 34, uint16_t(26 * text.size()));
    Bytes data(line + lineSize);
    std::copy(header.begin(), header.end(), data.begin());
    std::copy(text.begin(), text.end(), data.begin() + textStart);
    data[textStart + text.size()] = paragraphEnd;
    std::copy(trailer.begin(), trailer.end(), data.begin() + line);
    return data;
}

bool ReplacePwiNoteText(Bytes& data, std::string_view text) {
    if (!ReadPwiNote(data)) return false;
    auto replacement = WritePwiNote(text);
    if (!replacement) return false;
    SetWord(*replacement, 0x01a, Word(data, 0x01a));
    data = std::move(*replacement);
    return true;
}

}  // namespace frondSync
