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

// One classic size: its last block and page work and the next block and page are refused.
static void Check(const M::Geometry& g) {
    const uint32_t blocks = g.blocks, pagesPerBlock = g.pagesPerBlock;
    const size_t imageBytes = size_t(g.PhysicalBytes());
    M card;
    M::Page input, untouched;
    untouched.fill(0x73);
    assert(card.Read(0, 0, untouched) == M::Result::absent && untouched[0] == 0x73);
    assert(card.Erase(0) == M::Result::absent);
    assert(card.RestoreCheckpoint({}) == M::Result::absent);
    std::vector<uint8_t> exported{0x32};
    assert(card.CopyImage(exported) == M::Result::absent && exported == std::vector<uint8_t>{0x32});

    std::vector<uint8_t> source(imageBytes, 0xff);
    assert(card.Attach(source, false) == M::Result::ok);
    assert(card.Present() && !card.Dirty());
    assert(card.Blocks() == blocks && card.PagesPerBlock() == pagesPerBlock && card.ImageBytes() == imageBytes);
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
    assert(source[(3 * pagesPerBlock + 7) * M::recordBytes] == 0xff);

    // Refused replacements and malformed checkpoints retain both bytes and the physical switch.
    card.SetWriteProtected(true);
    auto invalid = card.SaveCheckpoint();
    invalid.writeProtected = false;
    invalid.pin.reset();
    assert(card.RestoreCheckpoint(invalid) == M::Result::invalidImage);
    assert(card.WriteProtected());
    std::vector<uint8_t> wrongSize(512, 0);
    assert(card.Attach(wrongSize, false) == M::Result::invalidImage);
    std::vector<uint8_t> nearlyRight(imageBytes - M::recordBytes, 0xff);
    assert(card.Attach(nearlyRight, false) == M::Result::invalidImage);
    assert(card.Blocks() == blocks);
    assert(card.WriteProtected() && card.Dirty());
    assert(card.Program(3, 7, input, true, true) == M::Result::writeProtected);
    assert(card.Erase(3) == M::Result::writeProtected);
    ExpectPage(card, 3, 7, 0xa5, 0xff);
    card.SetWriteProtected(false);

    for (auto address : {blocks, std::numeric_limits<uint32_t>::max()}) {
        assert(card.Read(address, 0, untouched) == M::Result::address && untouched[0] == 0x73);
        assert(card.Program(address, 0, input, true, true) == M::Result::address);
        assert(card.Erase(address) == M::Result::address);
    }
    assert(card.Read(0, pagesPerBlock, untouched) == M::Result::address);
    assert(card.Read(blocks - 1, pagesPerBlock, untouched) == M::Result::address);
    assert(card.Erase(blocks) == M::Result::address);
    assert(card.Program(0, pagesPerBlock, input, true, true) == M::Result::address);
    ExpectPage(card, 3, 7, 0xa5, 0xff);

    // Whole-block erase includes extra bytes and leaves adjacent blocks intact.
    input.fill(0);
    for (unsigned page = 0; page < pagesPerBlock; ++page)
        assert(card.Program(3, page, input, true, true) == M::Result::ok);
    assert(card.Program(4, 0, input, true, true) == M::Result::ok);
    assert(card.Erase(3) == M::Result::ok);
    for (unsigned page = 0; page < pagesPerBlock; ++page) ExpectPage(card, 3, page, 0xff, 0xff);
    ExpectPage(card, 4, 0, 0, 0);
    assert(card.ChangedPages() == 1);

    // Export/reopen retains physical mapping bytes, including the last page and its last extra byte.
    input.fill(0xc3);
    input.back() = 0x42;
    assert(card.Program(blocks - 1, pagesPerBlock - 1, input, true, true) == M::Result::ok);
    assert(card.CopyImage(exported) == M::Result::ok);
    assert(exported.size() == imageBytes && exported.back() == 0x42);
    auto oldAttachment = card.SaveCheckpoint();
    card.Detach();
    assert(!card.Present() && !card.Dirty() && !card.WriteProtected());
    assert(card.Attach(exported, false) == M::Result::ok);
    assert(!card.Dirty());
    assert(card.RestoreCheckpoint(oldAttachment) == M::Result::invalidImage);
    ExpectPage(card, 4, 0, 0, 0);
    M::Page last;
    assert(card.Read(blocks - 1, pagesPerBlock - 1, last) == M::Result::ok && last == input);
    assert(exported.size() - M::recordBytes == (size_t(blocks) * pagesPerBlock - 1) * M::recordBytes);
    // Erasing a non-erased base must be represented in the exported delta too.
    assert(card.Erase(4) == M::Result::ok && card.Dirty());
    ExpectPage(card, 4, 0, 0xff, 0xff);
    assert(card.CopyImage(source) == M::Result::ok);
    assert(source[(4 * pagesPerBlock) * M::recordBytes] == 0xff);
    assert(exported[(4 * pagesPerBlock) * M::recordBytes] == 0);
    card.Detach();
    assert(card.Blocks() == frondMsGeometry::largest.blocks);
}

int main() {
    static_assert(frondMsGeometry::largest.PhysicalBytes() == 136577024);
    static_assert(M::maxImageBytes == 136577024);
    static_assert(!std::is_copy_constructible_v<M> && !std::is_copy_assignable_v<M>);
    static_assert(!std::is_move_constructible_v<M> && !std::is_move_assignable_v<M>);
    // The plan's table: 8 KB blocks for 4 and 8 MB, 16 KB from 16 MB.
    const uint64_t physical[] = {4268032, 8536064, 17072128, 34144256, 68288512, 136577024};
    const uint32_t effective[] = {496, 992, 992, 1984, 3968, 7936};
    for (unsigned i = 0; i < 6; ++i) {
        const auto& g = frondMsGeometry::sizes[i];
        assert(g.PhysicalBytes() == physical[i] && g.Effective() == effective[i]);
        assert(g.BlockKib() == (g.megabytes < 16 ? 8u : 16u));
        assert(frondMsGeometry::byPhysicalBytes(physical[i]) == &g && frondMsGeometry::byMegabytes(g.megabytes) == &g);
        Check(g);
    }
    assert(!frondMsGeometry::byMegabytes(256) && !frondMsGeometry::byMegabytes(2) && !frondMsGeometry::byPhysicalBytes(0));
    std::puts("Memory Stick physical media checks passed");
}
