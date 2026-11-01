#include "MemoryStickMedia.h"

#include <cassert>
#include <cstdio>
#include <limits>
#include <type_traits>

using M = MemoryStickMedia;

static void ExpectPage(const M& card, uint32_t block, uint32_t page, uint8_t data, uint8_t extra) {
    M::Page actual;
    assert(card.Read(block, page, actual) == M::Result::ok);
    for (unsigned i = 0; i < M::dataBytes; ++i) assert(actual[i] == data);
    for (unsigned i = M::dataBytes; i < M::recordBytes; ++i) assert(actual[i] == extra);
}

int main() {
    static_assert(M::imageBytes == 136577024);
    static_assert(!std::is_copy_constructible_v<M> && !std::is_copy_assignable_v<M>);
    static_assert(!std::is_move_constructible_v<M> && !std::is_move_assignable_v<M>);
    M card;
    M::Page input, untouched;
    untouched.fill(0x73);
    assert(card.Read(0, 0, untouched) == M::Result::absent && untouched[0] == 0x73);
    assert(card.Erase(0) == M::Result::absent);
    assert(card.RestoreCheckpoint({}) == M::Result::absent);
    std::vector<uint8_t> exported{0x32};
    assert(card.CopyImage(exported) == M::Result::absent && exported == std::vector<uint8_t>{0x32});

    std::vector<uint8_t> source(M::imageBytes, 0xff);
    assert(card.Attach(source, false) == M::Result::ok);
    assert(card.Present() && !card.Dirty());
    source[0] = 0x12;
    ExpectPage(card, 0, 0, 0xff, 0xff);
    source[0] = 0xff;

    input.fill(0xa5);
    assert(card.Program(3, 7, input, true, false) == M::Result::ok);
    ExpectPage(card, 3, 7, 0xa5, 0xff);
    assert(card.Dirty() && card.ChangedPages() == 1);
    const auto checkpoint = card.SaveCheckpoint();
    input.fill(0x5a);
    assert(card.Program(3, 7, input, false, true) == M::Result::ok);
    ExpectPage(card, 3, 7, 0xa5, 0x5a);
    assert(card.Program(3, 7, input, true, false) == M::Result::ok);
    ExpectPage(card, 3, 7, 0, 0x5a);
    input.fill(0xff);
    assert(card.Program(3, 7, input, true, true) == M::Result::ok);
    ExpectPage(card, 3, 7, 0, 0x5a);
    assert(card.RestoreCheckpoint(checkpoint) == M::Result::ok);
    ExpectPage(card, 3, 7, 0xa5, 0xff);
    assert(source[(3 * M::pagesPerBlock + 7) * M::recordBytes] == 0xff);

    // Refused replacements and malformed checkpoints retain both bytes and the physical switch.
    card.SetWriteProtected(true);
    auto invalid = card.SaveCheckpoint();
    invalid.writeProtected = false;
    invalid.changes[M::pageCount] = input;
    assert(card.RestoreCheckpoint(invalid) == M::Result::invalidImage);
    assert(card.WriteProtected());
    std::vector<uint8_t> wrongSize(512, 0);
    assert(card.Attach(wrongSize, false) == M::Result::invalidImage);
    assert(card.WriteProtected() && card.Dirty());
    assert(card.Program(3, 7, input, true, true) == M::Result::writeProtected);
    assert(card.Erase(3) == M::Result::writeProtected);
    ExpectPage(card, 3, 7, 0xa5, 0xff);
    card.SetWriteProtected(false);

    for (auto address : {M::blocks, std::numeric_limits<uint32_t>::max()}) {
        assert(card.Read(address, 0, untouched) == M::Result::address && untouched[0] == 0x73);
        assert(card.Program(address, 0, input, true, true) == M::Result::address);
        assert(card.Erase(address) == M::Result::address);
    }
    assert(card.Read(0, M::pagesPerBlock, untouched) == M::Result::address);
    assert(card.Program(0, M::pagesPerBlock, input, true, true) == M::Result::address);
    ExpectPage(card, 3, 7, 0xa5, 0xff);

    // Whole-block erase includes extra bytes and leaves adjacent blocks intact.
    input.fill(0);
    for (unsigned page = 0; page < M::pagesPerBlock; ++page)
        assert(card.Program(3, page, input, true, true) == M::Result::ok);
    assert(card.Program(4, 0, input, true, true) == M::Result::ok);
    assert(card.Erase(3) == M::Result::ok);
    for (unsigned page = 0; page < M::pagesPerBlock; ++page) ExpectPage(card, 3, page, 0xff, 0xff);
    ExpectPage(card, 4, 0, 0, 0);
    assert(card.ChangedPages() == 1);

    // Export/reopen retains physical mapping bytes, including the last page and its last extra byte.
    input.fill(0xc3);
    input.back() = 0x42;
    assert(card.Program(M::blocks - 1, M::pagesPerBlock - 1, input, true, true) == M::Result::ok);
    assert(card.CopyImage(exported) == M::Result::ok);
    assert(exported.size() == M::imageBytes && exported.back() == 0x42);
    auto oldAttachment = card.SaveCheckpoint();
    card.Detach();
    assert(!card.Present() && !card.Dirty() && !card.WriteProtected());
    assert(card.Attach(exported, false) == M::Result::ok);
    assert(!card.Dirty());
    assert(card.RestoreCheckpoint(oldAttachment) == M::Result::invalidImage);
    ExpectPage(card, 4, 0, 0, 0);
    M::Page last;
    assert(card.Read(M::blocks - 1, M::pagesPerBlock - 1, last) == M::Result::ok && last == input);
    // Erasing a non-erased base must be represented in the exported delta too.
    assert(card.Erase(4) == M::Result::ok && card.Dirty());
    ExpectPage(card, 4, 0, 0xff, 0xff);
    assert(card.CopyImage(source) == M::Result::ok);
    assert(source[(4 * M::pagesPerBlock) * M::recordBytes] == 0xff);
    assert(exported[(4 * M::pagesPerBlock) * M::recordBytes] == 0);
    std::puts("Memory Stick physical media checks passed");
}
