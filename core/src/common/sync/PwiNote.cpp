#include "PwiNote.h"

#include "PimRecord.h"

#include <algorithm>
#include <array>
#include <utility>

namespace frondSync {
namespace {

constexpr size_t indexStart = 0x146;
constexpr size_t indexEntrySize = 8;
constexpr size_t paragraphHeaderSize = 36;
constexpr size_t lineHeaderSize = 6;
constexpr size_t lineSize = 10;
constexpr uint8_t paragraphEnd = 0xc4;
constexpr uint8_t latin1Escape = 0xc1;
constexpr uint8_t euroFirst = 0xac;
constexpr uint8_t euroSecond = 0x82;
constexpr uint8_t palmEuro = 0x80;
constexpr uint16_t lineHeight = 0x030d;
constexpr uint16_t firstParagraphId = 0x23;
constexpr std::array<uint8_t, 18> footer{0x82, 0, 9, 0, 3, 0, 0x82, 0};

uint16_t Word(const Bytes& data, size_t offset) {
    return data[offset] | (uint16_t(data[offset + 1]) << 8);
}

// Fixed words in the observed default-font Notes header.
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
    {0x128, 0x40}, {0x12a, 8},
};

bool PlainAscii(uint8_t c) { return c >= 0x20 && c <= 0x7e; }

template <typename Buffer>
void SetWord(Buffer& buffer, size_t offset, uint16_t value) {
    buffer[offset] = value & 0xff;
    buffer[offset + 1] = value >> 8;
}

std::array<uint8_t, indexStart> Header(uint16_t paragraphs, uint16_t chars, uint16_t lines, uint16_t setting) {
    std::array<uint8_t, indexStart> header{};
    for (const auto& [offset, value] : fixedWords) SetWord(header, offset, value);
    SetWord(header, 0x01a, setting);
    SetWord(header, 0x12c, 6 + 2 * paragraphs);
    SetWord(header, 0x12e, paragraphs);
    SetWord(header, 0x132, paragraphs);
    SetWord(header, 0x136, chars);
    SetWord(header, 0x13a, lines);
    return header;
}

std::array<uint8_t, paragraphHeaderSize> ParagraphHeader(uint16_t count, uint16_t encodedCount,
                                                        uint16_t id, uint16_t words, uint16_t lines,
                                                        uint16_t lineId) {
    std::array<uint8_t, paragraphHeaderSize> header{};
    SetWord(header, 0, 0x41);
    SetWord(header, 2, id);
    SetWord(header, 4, words);
    SetWord(header, 6, count);
    SetWord(header, 8, encodedCount + 7);
    SetWord(header, 10, lines);
    SetWord(header, 14, lineId);
    SetWord(header, 30, 0x01e5);
    SetWord(header, 32, 0xe600);
    SetWord(header, 34, 0x0a);
    return header;
}

size_t RoundToWord(size_t bytes) { return (bytes + 3) & ~size_t(3); }

bool Zeroes(Bytes::const_iterator first, Bytes::const_iterator last) {
    return std::all_of(first, last, [](uint8_t byte) { return byte == 0; });
}

bool ReadParagraph(const Bytes& data, size_t index, size_t& position, std::string& text) {
    const auto lines = Word(data, index);
    const auto count = Word(data, index + 2);
    const auto id = Word(data, index + 4);
    if (lines == 0 || count < lines || count > UINT16_MAX - 7 || Word(data, index + 6) != 0 ||
        data.size() - position < paragraphHeaderSize)
        return false;

    const auto words = Word(data, position + 4);
    const auto encodedSize = Word(data, position + 8);
    if (encodedSize < count + 7 || encodedSize > 2 * count + 6) return false;
    const auto encodedCount = uint16_t(encodedSize - 7);
    const auto lineId = Word(data, position + 14);
    const auto paragraph = ParagraphHeader(count, encodedCount, id, words, lines, lineId);
    if (!std::equal(paragraph.begin(), paragraph.end(), data.begin() + position)) return false;
    const size_t textStart = position + paragraphHeaderSize;
    const size_t textEnd = textStart + encodedCount;
    const size_t line = position + 6 + 4 * size_t(words);
    const size_t payloadSize = RoundToWord(lineSize * lines);
    if (line < textEnd || line - textEnd > 7 || line > data.size() ||
        data.size() - line < lineHeaderSize + payloadSize)
        return false;
    if (data[textEnd - 1] != paragraphEnd || !Zeroes(data.begin() + textEnd, data.begin() + line) ||
        Word(data, line) != 0x42 || Word(data, line + 2) != lineId ||
        Word(data, line + 4) != payloadSize / 4)
        return false;

    size_t lineCount = 0;
    position = line + lineHeaderSize;
    for (size_t i = 0; i < lines; ++i, position += lineSize) {
        const auto chars = Word(data, position);
        if (chars == 0 || Word(data, position + 2) != 0 || Word(data, position + 4) != 0 ||
            Word(data, position + 8) != lineHeight)
            return false;
        lineCount += chars;
    }
    const size_t lineEnd = line + lineHeaderSize + payloadSize;
    if (lineCount != count || !Zeroes(data.begin() + position, data.begin() + lineEnd)) return false;
    position = lineEnd;

    Bytes content;
    content.reserve(count - 1);
    for (size_t i = textStart; i < textEnd - 1; ++i) {
        const auto byte = data[i];
        if (PlainAscii(byte))
            content.push_back(byte);
        else if (byte == latin1Escape && i + 1 < textEnd - 1 && data[i + 1] >= 0xa0)
            content.push_back(data[++i]);
        else if (byte == euroFirst && i + 1 < textEnd - 1 && data[i + 1] == euroSecond) {
            content.push_back(palmEuro);
            ++i;
        } else {
            return false;
        }
    }
    if (content.size() + 1 != count) return false;
    text += PalmPimText(content);
    return true;
}

// Break long paragraphs at spaces when possible. Notes recalculates the layout on save.
std::vector<uint16_t> LineCounts(std::string_view text) {
    constexpr size_t maxColumns = 30;
    std::vector<uint16_t> result;
    while (text.size() > maxColumns) {
        const size_t space = text.rfind(' ', maxColumns - 1);
        const size_t count = space == std::string_view::npos ? maxColumns : space + 1;
        result.push_back(uint16_t(count));
        text.remove_prefix(count);
    }
    result.push_back(uint16_t(text.size() + 1));
    return result;
}

bool WriteParagraph(Bytes& data, size_t index, std::string_view text, uint16_t id,
                    const std::vector<uint16_t>& lines) {
    const auto count = uint16_t(text.size() + 1);
    const size_t encodedCount = count + std::count_if(text.begin(), text.end(), [](unsigned char c) {
        return !PlainAscii(c);
    });
    if (encodedCount > UINT16_MAX - 7) return false;
    SetWord(data, index, uint16_t(lines.size()));
    SetWord(data, index + 2, count);
    SetWord(data, index + 4, id);

    // The text and its terminator are followed by up to seven zero bytes.
    const size_t start = data.size();
    const size_t textStart = start + paragraphHeaderSize;
    const size_t line = textStart + 2 + ((encodedCount + 5) & ~size_t(7));
    const auto words = uint16_t((line - start - 6) / 4);
    const auto paragraph = ParagraphHeader(count, uint16_t(encodedCount), id, words, uint16_t(lines.size()), id - 1);
    const size_t payloadSize = RoundToWord(lineSize * lines.size());
    data.resize(line + lineHeaderSize + payloadSize);
    std::copy(paragraph.begin(), paragraph.end(), data.begin() + start);
    size_t offset = textStart;
    for (const uint8_t c : text) {
        if (c == palmEuro) {
            data[offset++] = euroFirst;
            data[offset++] = euroSecond;
        } else {
            if (!PlainAscii(c)) data[offset++] = latin1Escape;
            data[offset++] = c;
        }
    }
    data[offset] = paragraphEnd;

    SetWord(data, line, 0x42);
    SetWord(data, line + 2, id - 1);
    SetWord(data, line + 4, uint16_t(payloadSize / 4));
    size_t position = line + lineHeaderSize;
    for (size_t i = 0; i < lines.size(); ++i, position += lineSize) {
        SetWord(data, position, lines[i]);
        SetWord(data, position + 6, uint16_t(26 * (lines[i] - (i + 1 == lines.size()))));
        SetWord(data, position + 8, lineHeight);
    }
    return true;
}

}  // namespace

std::optional<std::string> ReadPwiNote(const Bytes& data) {
    if (data.size() < indexStart) return std::nullopt;
    const auto setting = Word(data, 0x01a);
    if (setting != 100 && setting != 200) return std::nullopt;
    const auto paragraphs = Word(data, 0x12e);
    if (paragraphs == 0 || 6 + 2 * paragraphs > UINT16_MAX) return std::nullopt;
    const size_t indexEnd = indexStart + paragraphs * indexEntrySize;
    if (indexEnd > data.size()) return std::nullopt;
    size_t position = indexEnd;

    const auto header = Header(paragraphs, Word(data, 0x136), Word(data, 0x13a), setting);
    if (!std::equal(header.begin(), header.end(), data.begin())) return std::nullopt;
    std::string text;
    size_t chars = 0, lines = 0;
    for (size_t index = indexStart; index < indexEnd; index += indexEntrySize) {
        if (index != indexStart) text += '\n';
        if (!ReadParagraph(data, index, position, text)) return std::nullopt;
        lines += Word(data, index);
        chars += Word(data, index + 2);
    }
    if (chars != Word(data, 0x136) || lines != Word(data, 0x13a) ||
        RoundToWord(position + footer.size()) != data.size() ||
        !std::equal(footer.begin(), footer.end(), data.begin() + position) ||
        !Zeroes(data.begin() + position + footer.size(), data.end()))
        return std::nullopt;
    return text;
}

std::optional<Bytes> WritePwiNote(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > 3 * (UINT16_MAX - 8)) return std::nullopt;
    Bytes encoded;
    if (!EncodePalmPimText(std::string(utf8), encoded) || encoded.size() >= UINT16_MAX - 7 ||
        std::any_of(encoded.begin(), encoded.end(), [](uint8_t c) {
            return c != '\n' && !PlainAscii(c) && c != palmEuro && c < 0xa0;
        }))
        return std::nullopt;
    const std::string_view text(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    const auto paragraphs = size_t(std::count(text.begin(), text.end(), '\n')) + 1;
    if (paragraphs > (UINT16_MAX - firstParagraphId) / 2) return std::nullopt;

    size_t lines = 0;
    Bytes data(indexStart + paragraphs * indexEntrySize);
    size_t begin = 0;
    for (size_t i = 0; i < paragraphs; ++i) {
        const auto end = text.find('\n', begin);
        const auto paragraph = text.substr(begin, end - begin);
        const auto wraps = LineCounts(paragraph);
        lines += wraps.size();
        if (!WriteParagraph(data, indexStart + i * indexEntrySize, paragraph, uint16_t(firstParagraphId + 2 * i), wraps))
            return std::nullopt;
        if (end != std::string_view::npos) begin = end + 1;
    }

    const auto header = Header(uint16_t(paragraphs), uint16_t(text.size() + 1), uint16_t(lines), 200);
    std::copy(header.begin(), header.end(), data.begin());
    data.insert(data.end(), footer.begin(), footer.end());
    data.resize(RoundToWord(data.size()));
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

void DetachPwiMemo(PimRecord& memo) {
    memo.extras.erase(noteFileField);
    memo.extras.erase(noteTextField);
}

}  // namespace frondSync
