#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "PwiNote.h"

using namespace frondSync;

namespace {

Bytes Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void CheckWriter() {
    for (size_t length = 1; length <= 21; ++length) {
        const std::string text(length, 'W');
        const auto note = WritePwiNote(text);
        assert(note && ReadPwiNote(*note) == text);
    }
    assert(!WritePwiNote(""));
    assert(!WritePwiNote(std::string(65536, 'x')));
    for (const std::string text : {"first\nsecond", "\nfirst\n\nsecond\n", "one long line will wrap at the next space",
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                   "caf\xc3\xa9\n\xe2\x82\xac \xc2\xa2"}) {
        const auto note = WritePwiNote(text);
        assert(note && ReadPwiNote(*note) == text);
    }
    assert(!WritePwiNote("first\rsecond"));
    assert(!WritePwiNote("tab\there"));
    assert(!WritePwiNote(std::string_view("a\0b", 3)));
    assert(!WritePwiNote("unsupported \xd0\x96"));

    auto data = *WritePwiNote("Frond note test");
    assert(data.size() == 424);
    for (size_t size = 0; size < data.size(); ++size)
        assert(!ReadPwiNote(Bytes(data.begin(), data.begin() + size)));

    auto malformed = data;
    malformed.push_back(0);
    assert(!ReadPwiNote(malformed));
    const auto unsupported = malformed;
    assert(!ReplacePwiNoteText(malformed, "Short"));
    assert(malformed == unsupported);
    malformed = data;
    malformed[0x136] ^= 1;
    assert(!ReadPwiNote(malformed));
    malformed = data;
    malformed[0x172] = 0xe5;
    assert(!ReadPwiNote(malformed));

    data[0x1a] = 100;
    const auto original = data;
    assert(!ReplacePwiNoteText(data, "Frond\rnote test"));
    assert(data == original);
    assert(ReplacePwiNoteText(data, "Frond note test beta"));
    assert(data.size() == 432);
    assert(data[0x1a] == 100);
    assert(ReadPwiNote(data) == "Frond note test beta");
    assert(ReplacePwiNoteText(data, "Short"));
    assert(ReadPwiNote(data) == "Short");
    assert(data.size() < original.size());
    assert(ReplacePwiNoteText(data, "First line\n\nThird"));
    assert(ReadPwiNote(data) == "First line\n\nThird");
    assert(data[0x1a] == 100);

    // Each paragraph's line count must agree with the index and document totals.
    malformed = data;
    malformed[0x146] ^= 1;
    assert(!ReadPwiNote(malformed));
    malformed = data;
    malformed[0x13a] ^= 1;
    assert(!ReadPwiNote(malformed));
}

void CheckFixtures(const std::filesystem::path& fixtures) {
    auto before = Read(fixtures / "typed-before.pwi");
    assert(ReadPwiNote(before) == "Synthetic alpha");
    assert(ReadPwiNote(Read(fixtures / "typed-after.pwi")) == "Synthetic alpha beta");
    assert(ReadPwiNote(Read(fixtures / "copied-back-saved.pwi")) == "Synthetic alpha gamma");
    assert(ReplacePwiNoteText(before, "Synthetic alpha beta"));
    assert(before.size() == 432);
    assert(ReadPwiNote(before) == "Synthetic alpha beta");

    auto ink = Read(fixtures / "ink.pwi");
    const auto originalInk = ink;
    assert(!ReadPwiNote(ink));
    assert(!ReplacePwiNoteText(ink, "Frond note test"));
    assert(ink == originalInk);
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 1 || argc == 2 || argc == 6 || argc == 7);
    CheckWriter();
    if (argc >= 2) CheckFixtures(argv[1]);
    if (argc >= 6) {
        assert(ReadPwiNote(Read(argv[2])) == "First example line\n\nThird line with more than twenty one letters");
        assert(ReadPwiNote(Read(argv[3])) == "\xc3\x89ut");
        assert(ReadPwiNote(Read(argv[4])) == "Currency \xe2\x82\xac\xc2\xa2");
        const std::string generatedText = "Generated first line needs to wrap before the accented line\n\n"
                                          "Caf\xc3\xa9 \xe2\x82\xac";
        assert(WritePwiNote(generatedText) == Read(argv[5]));
        if (argc == 7) assert(ReadPwiNote(Read(argv[6])) == generatedText + " ok");
    }
}
