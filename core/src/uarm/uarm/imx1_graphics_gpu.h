#ifndef FROND_IMX1_GRAPHICS_GPU_H
#define FROND_IMX1_GRAPHICS_GPU_H

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "imx1_graphics_memory.h"

// A host GPU that keeps a copy of both graphics memory banks and draws engine fills on it. Each bank is a texture
// of 16-bit texels, 2048 wide, so one texture row is one 4 KB page. Commands run in order on the backend's own
// thread and each gets a number, counting up from 1. After each batch of commands the backend reads the pages it
// changed back into the replica, a host copy of the GPU's banks, and confirms the batch's last number.
class Imx1GraphicsGpuBackend {
   public:
    static constexpr uint32_t rowTexels = 2048, pageBytes = 4096;
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    using Pixels = std::shared_ptr<const std::vector<uint16_t>>;
    struct Stats {
        // Written by the backend's thread and read by others, so atomic (relaxed).
        std::atomic<uint64_t> batches{0}, readbacks{0}, readbackBytes{0};
        // Worker host time from posting to running each command, drawing, and reading batches back.
        std::atomic<uint64_t> queueNs{0}, drawNs{0}, readNs{0};
    };
    virtual ~Imx1GraphicsGpuBackend() = default;
    virtual const std::string& renderer() const = 0;
    // The two banks' replicas, each the bank's size. Only the backend's thread writes them, at a confirm, and only
    // pages its batch changed.
    virtual void setReplica(uint8_t* bank0, uint8_t* bank1) = 0;
    // pages whole pages of bank from firstPage.
    virtual uint64_t upload(unsigned bank, uint32_t firstPage, uint32_t pages, Bytes data) = 0;
    // A fill, with its inline pixels if it has any. It changes the pages from firstPage.
    virtual uint64_t fill(const Imx1GpuFill& fill, Pixels pixels, uint32_t firstPage, uint32_t pages) = 0;
    // Writes CPU bytes into pages whole pages of bank from firstPage: data holds the pages' bytes, then a byte for
    // each that is nonzero where the byte is to be written.
    virtual uint64_t patch(unsigned bank, uint32_t firstPage, uint32_t pages, Bytes data) = 0;
    // The last command whose pages the replica holds. Any thread.
    virtual uint64_t confirmed() const = 0;
    // Waits until command is confirmed; false if the backend broke first.
    virtual bool waitConfirmed(uint64_t command) = 0;
    // Waits until the backend's thread has taken every command posted, run or dropped.
    virtual void drain() = 0;
    // False once the backend has hit an error it can't draw past. Nothing after its last confirm counts then.
    virtual bool healthy() const = 0;
    virtual const Stats& backendStatistics() const = 0;
    // Tests: a GL error while running command, and holding commands back so the next batch takes them all.
    virtual void injectError(uint64_t command) = 0;
    virtual void hold(bool held) = 0;
};

// Keeps graphics memory the truth with a GPU copy beside it (Fast only). Pages start newer in the bytes; a fill
// uploads the pages it uses that are, draws, and leaves its destination pages newer on the GPU. Those come back
// from the replica when the CPU reads them (an access, a display row, a state save, a hash), waiting only if the
// backend hasn't confirmed the page's last command yet. A CPU write onto a GPU-newer page goes into the bytes and
// a byte mask instead of waiting; the masked bytes are patched into the GPU copy before its next use of the page
// and win over the replica when the page comes back. If the backend breaks, the commands since its last confirm
// are run again on the replica on the CPU, every page comes back and fills stay on the CPU from then on.
// FROND_ZODIAC_GPU picks the mode: on (or 1), diff (draws on both and checks every GPU fill's pages against a
// CPU-only shadow copy), anything else off.
class Imx1GraphicsGpu final : public Imx1GraphicsResidency {
   public:
    enum class Mode { off, on, differential };
    static Mode modeFromEnvironment() {
        const char* value = std::getenv("FROND_ZODIAC_GPU");
        if (!value) return Mode::off;
        if (!std::strcmp(value, "on") || !std::strcmp(value, "1")) return Mode::on;
        if (!std::strcmp(value, "diff")) return Mode::differential;
        return Mode::off;
    }
    struct Stats {
        uint64_t fills = 0, solid = 0, inlinePixels = 0, copies = 0, keyed = 0, doubled = 0, inverts = 0, a4 = 0;
        uint64_t uploads = 0, uploadPages = 0, patches = 0, patchPages = 0, downloads = 0, downloadPages = 0;
        uint64_t downloadsFor[causes] = {}, waitsFor[causes] = {}, waits = 0;
        // CPU writes onto GPU-newer pages, and CPU reads answered from their own masked bytes.
        uint64_t maskedWrites = 0, maskedReads = 0;
        // Host time the emulation thread spent waiting for confirms, and in fill and upload calls.
        uint64_t waitNs = 0, callNs = 0;
        uint64_t compared = 0, mismatches = 0;
        // Backend failures recovered from, and the commands run again on the CPU for them.
        uint64_t recoveries = 0, replayed = 0;
    };
    Imx1GraphicsGpu(std::unique_ptr<Imx1GraphicsGpuBackend> gpu, bool differential) : backend(std::move(gpu)) {
        mirror = differential;
    }
    ~Imx1GraphicsGpu() override {
        if (!std::getenv("FROND_ZODIAC_GPU_QUIET")) report(stderr);
        // Its thread may still be writing the replica.
        backend.reset();
    }
    Imx1GraphicsGpu(const Imx1GraphicsGpu&) = delete;
    Imx1GraphicsGpu& operator=(const Imx1GraphicsGpu&) = delete;
    const std::string& renderer() const { return backend->renderer(); }
    const Stats& statistics() const { return stats; }
    Imx1GraphicsGpuBackend& gpuBackend() { return *backend; }
    Imx1GraphicsMemory* attachedMemory() const { return memory; }
    void report(FILE* out) const {
        std::fprintf(out, "zodiac gpu: %s, %" PRIu64 " fills (solid %" PRIu64 ", inline %" PRIu64 ", copy %" PRIu64
                     ", keyed %" PRIu64 ", doubled %" PRIu64 ", invert %" PRIu64 ", a4 %" PRIu64 "), uploads %" PRIu64
                     " (%" PRIu64 " pages), patches %" PRIu64 " (%" PRIu64 " pages), downloads %" PRIu64 " (%" PRIu64
                     " pages, %" PRIu64 " waits, %.1f ms waiting, %.1f ms in calls)",
                     backend->renderer().c_str(), stats.fills, stats.solid, stats.inlinePixels, stats.copies,
                     stats.keyed, stats.doubled, stats.inverts, stats.a4, stats.uploads, stats.uploadPages,
                     stats.patches, stats.patchPages, stats.downloads, stats.downloadPages, stats.waits,
                     stats.waitNs / 1e6, stats.callNs / 1e6);
        std::fprintf(out, " (read %" PRIu64 ", write %" PRIu64 ", span %" PRIu64 ", copy %" PRIu64 ", whole %" PRIu64 ", writes %" PRIu64
                     "), %" PRIu64 " fills on the CPU (%" PRIu64 " small)", stats.downloadsFor[access], stats.downloadsFor[accessWrite], stats.downloadsFor[span],
                     stats.downloadsFor[copy], stats.downloadsFor[whole], stats.downloadsFor[writes], cpuFills, smallFills);
        std::fprintf(out, ", waits for read %" PRIu64 ", span %" PRIu64 ", copy %" PRIu64 ", whole %" PRIu64
                     ", masked writes %" PRIu64 ", masked reads %" PRIu64, stats.waitsFor[access], stats.waitsFor[span],
                     stats.waitsFor[copy], stats.waitsFor[whole], stats.maskedWrites, stats.maskedReads);
        const auto& b = backend->backendStatistics();
        std::fprintf(out, ", %" PRIu64 " batches, %" PRIu64 " readbacks (%.1f MB)", b.batches.load(std::memory_order_relaxed),
                     b.readbacks.load(std::memory_order_relaxed), b.readbackBytes.load(std::memory_order_relaxed) / 1e6);
        if (stats.recoveries) std::fprintf(out, ", %" PRIu64 " recoveries (%" PRIu64 " commands replayed)",
                                           stats.recoveries, stats.replayed);
        if (mirror) std::fprintf(out, ", compared %" PRIu64 ", mismatches %" PRIu64, stats.compared, stats.mismatches);
        std::fprintf(out, "\n");
    }
    // Takes memory as it is: every page newer in the bytes, and in differential mode a fresh shadow copy.
    void attach(Imx1GraphicsMemory* graphics) {
        detach();
        memory = graphics;
        gpuNewerPages = 0;
        for (unsigned bank = 0; bank < 2; ++bank) {
            uint32_t size = 0;
            memory->rawBank(bank, size);
            pages[bank].assign(size / pageBytes, cpuNewer);
            need[bank].assign(size / pageBytes, 0);
            drawnFirst[bank].assign(size / pageBytes, pageBytes);
            drawnEnd[bank].assign(size / pageBytes, 0);
            slot[bank].assign(size / pageBytes, -1);
            if (replica[bank].size() != size) replica[bank].assign(size, 0);
        }
        masks.clear();
        freeSlots.clear();
        log.clear();
        backend->setReplica(replica[0].data(), replica[1].data());
        if (mirror) {
            if (!shadow) shadow = std::make_unique<Imx1GraphicsMemory>();
            for (unsigned bank = 0; bank < 2; ++bank) {
                uint32_t size = 0, shadowSize = 0;
                const uint8_t* from = memory->rawBank(bank, size);
                uint8_t* to = shadow->rawBank(bank, shadowSize);
                std::memcpy(to, from, std::min(size, shadowSize));
            }
        }
        memory->setResidency(this);
    }
    // Brings every GPU-newer page back and lets the memory go.
    void detach() {
        if (!memory) return;
        cause = whole;
        for (unsigned bank = 0; bank < 2; ++bank) cpuReads(bank, 0, uint32_t(pages[bank].size()) * pageBytes);
        backend->drain();
        log.clear();
        memory->setResidency(nullptr);
        memory = nullptr;
    }
    void cpuReads(unsigned bank, uint32_t first, uint32_t end) override {
        if (!gpuNewerPages || end <= first) return;
        const uint32_t last = std::min<uint32_t>((end - 1) / pageBytes, uint32_t(pages[bank].size()) - 1);
        uint32_t size = 0;
        uint8_t* bytes = memory->rawBank(bank, size);
        // A small read of bytes the CPU itself wrote since the page went to the GPU needs nothing from it.
        if (end - first <= 4 && first / pageBytes == last && pages[bank][last] == gpuNewer && slot[bank][last] >= 0) {
            const uint8_t* mask = masks[size_t(slot[bank][last])].data();
            bool all = true;
            for (uint32_t i = first; i < end; ++i) all = all && mask[i % pageBytes];
            if (all) { ++stats.maskedReads; return; }
        }
        // A GPU-newer page's bytes are stale only where the GPU drew since it came back.
        auto stale = [&](uint32_t page) {
            if (pages[bank][page] != gpuNewer) return false;
            const uint32_t start = page * pageBytes;
            return std::max(first, start + drawnFirst[bank][page]) < std::min(end, start + drawnEnd[bank][page]);
        };
        for (uint32_t page = first / pageBytes; page <= last;) {
            if (!stale(page)) { ++page; continue; }
            uint32_t run = page;
            uint64_t wanted = 0;
            while (run <= last && stale(run)) wanted = std::max(wanted, need[bank][run++]);
            if (!confirm(wanted)) return;  // Recovered: every page is back.
            for (uint32_t i = page; i < run; ++i) {
                drawnFirst[bank][i] = pageBytes;
                drawnEnd[bank][i] = 0;
                uint8_t* to = bytes + size_t(i) * pageBytes;
                const uint8_t* from = replica[bank].data() + size_t(i) * pageBytes;
                if (slot[bank][i] < 0) {
                    std::memcpy(to, from, pageBytes);
                    pages[bank][i] = same;
                } else {
                    // The CPU's own writes win. The GPU copy lacks only those, so the page keeps its mask and the
                    // next GPU use patches them in instead of uploading the page.
                    merge(to, from, masks[size_t(slot[bank][i])].data());
                    pages[bank][i] = same;
                }
            }
            ++stats.downloads;
            ++stats.downloadsFor[cause];
            stats.downloadPages += run - page;
            gpuNewerPages -= run - page;
            page = run;
        }
    }
    void cpuWrites(unsigned bank, uint32_t first, uint32_t end) override {
        if (end <= first) return;
        const uint32_t last = std::min<uint32_t>((end - 1) / pageBytes, uint32_t(pages[bank].size()) - 1);
        for (uint32_t page = first / pageBytes; page <= last; ++page) {
            if (pages[bank][page] != gpuNewer) {
                // The page goes up whole before the GPU next uses it, masked bytes and all.
                if (pages[bank][page] == same && slot[bank][page] >= 0) release(bank, page);
                pages[bank][page] = cpuNewer;
                continue;
            }
            if (slot[bank][page] < 0) {
                if (freeSlots.empty()) {
                    freeSlots.push_back(int32_t(masks.size()));
                    masks.emplace_back(pageBytes, 0);
                }
                slot[bank][page] = freeSlots.back();
                freeSlots.pop_back();
            }
            uint8_t* mask = masks[size_t(slot[bank][page])].data();
            const uint32_t from = std::max(first, page * pageBytes), to = std::min(end, (page + 1) * pageBytes);
            std::memset(mask + from % pageBytes, 1, to - from);
            ++stats.maskedWrites;
        }
    }
    void mirrorWrite(unsigned bank, uint32_t offset, uint32_t size, uint32_t value) override {
        uint32_t bytes = 0;
        uint8_t* to = shadow->rawBank(bank, bytes);
        for (uint32_t i = 0; i < size && offset + i < bytes; ++i) to[offset + i] = uint8_t(value >> (i * 8));
    }
    bool fill(const Imx1GpuFill& fill) override {
        if (lost) return false;
        if (!backend->healthy()) {
            recover();
            return false;
        }
        const uint32_t end = fill.end();
        // A small fill on bytes the GPU hasn't drawn costs less on the CPU than a trip to the GPU.
        if (uint64_t(fill.width) * fill.height < minPixels && !gpuDrew(fill.bank, fill.base, end) &&
            !((fill.copy || fill.a4) && gpuDrew(fill.sourceBank, fill.sourceBase, fill.sourceBase + fill.sourceLength))) {
            ++smallFills;
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        // The pixels a keyed copy writes depend on its source, so the CPU reads it to count them as the CPU loop does.
        uint64_t writes = uint64_t(fill.width) * fill.height;
        uint32_t dirtyFirst = fill.base, dirtyEnd = end;
        if (fill.keyed) {
            cause = copy;
            cpuReads(fill.sourceBank, fill.sourceBase, fill.sourceBase + fill.sourceLength);
            if (lost || !backend->healthy()) return false;  // Recovered: the CPU does this fill.
            keyedWrites(fill, writes, dirtyFirst, dirtyEnd);
            if (!writes) { ++stats.fills; ++stats.keyed; return true; }
        }
        prepare(fill.bank, fill.base, end);
        if (fill.copy || fill.a4) prepare(fill.sourceBank, fill.sourceBase, fill.sourceBase + fill.sourceLength);
        const uint32_t firstPage = fill.base / pageBytes, last = (end - 1) / pageBytes;
        Imx1GraphicsGpuBackend::Pixels pixels;
        if (fill.pixels)
            pixels = std::make_shared<const std::vector<uint16_t>>(fill.pixels, fill.pixels + size_t(fill.width) * fill.height);
        Imx1GpuFill posted = fill;
        posted.pixels = nullptr;
        const uint64_t number = backend->fill(posted, pixels, firstPage, last - firstPage + 1);
        logCommand({number, Entry::fill, fill.bank, firstPage, last - firstPage + 1, nullptr, posted, pixels});
        for (uint32_t page = firstPage; page <= last; ++page) {
            if (pages[fill.bank][page] != gpuNewer) ++gpuNewerPages;
            pages[fill.bank][page] = gpuNewer;
            need[fill.bank][page] = number;
            const uint32_t start = page * pageBytes;
            drawnFirst[fill.bank][page] = uint16_t(std::min<uint32_t>(drawnFirst[fill.bank][page], std::max(fill.base, start) - start));
            drawnEnd[fill.bank][page] = uint16_t(std::max<uint32_t>(drawnEnd[fill.bank][page], std::min(end, start + pageBytes) - start));
        }
        memory->noteDirty(fill.bank, dirtyFirst, dirtyEnd, writes);
        stats.callNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        ++stats.fills;
        if (fill.a4) ++stats.a4;
        else if (fill.invert) ++stats.inverts;
        else if (fill.doubled) ++stats.doubled;
        else if (fill.keyed) ++stats.keyed;
        else if (fill.copy) ++stats.copies;
        else if (fill.pixels) ++stats.inlinePixels;
        else ++stats.solid;
        return true;
    }
    // The writes and dirty span the CPU loop makes for a keyed copy, from the source bytes.
    void keyedWrites(const Imx1GpuFill& fill, uint64_t& writes, uint32_t& first, uint32_t& end) {
        uint32_t size = 0;
        const uint8_t* source = memory->rawBank(fill.sourceBank, size) + fill.sourceBase;
        writes = 0;
        first = UINT32_MAX;
        end = 0;
        for (uint32_t y = 0; y < fill.height; ++y) {
            const uint8_t* row = source + (fill.doubled ? size_t((fill.sourceY + y) >> 1) : size_t(y)) * fill.sourcePitch;
            uint32_t low = 0, high = 0;
            for (uint32_t x = 0; x < fill.width; ++x) {
                const uint8_t* at = row + (fill.doubled ? ((fill.sourceX + x) >> 1) * 2 : x * 2);
                if ((uint32_t(at[0] | at[1] << 8) == fill.colour) != fill.keyEqual) continue;
                if (!high) low = x * 2;
                high = x * 2 + 2;
                ++writes;
            }
            if (high) {
                first = std::min(first, fill.base + y * fill.pitch + low);
                end = std::max(end, fill.base + y * fill.pitch + high);
            }
        }
    }
    Imx1GraphicsMemory* shadowMemory() override { return shadow.get(); }
    // Fills under this many pixels go to the CPU where the GPU hasn't drawn their bytes. FROND_ZODIAC_GPU_MIN_PIXELS
    // sets it; tests set it to 0.
    uint64_t minPixels = [] {
        const char* value = std::getenv("FROND_ZODIAC_GPU_MIN_PIXELS");
        return value ? uint64_t(std::strtoull(value, nullptr, 10)) : defaultMinPixels;
    }();
    static constexpr uint64_t defaultMinPixels = 0;
    // Fills left to the CPU for minPixels.
    uint64_t smallFills = 0;
    // Whether the GPU drew any byte between bank offsets first and end that the CPU hasn't had back.
    bool gpuDrew(unsigned bank, uint32_t first, uint32_t end) const {
        if (!gpuNewerPages || end <= first) return false;
        const uint32_t last = std::min<uint32_t>((end - 1) / pageBytes, uint32_t(pages[bank].size()) - 1);
        for (uint32_t page = first / pageBytes; page <= last; ++page) {
            const uint32_t start = page * pageBytes;
            if (pages[bank][page] == gpuNewer &&
                std::max(first, start + drawnFirst[bank][page]) < std::min(end, start + drawnEnd[bank][page]))
                return true;
        }
        return false;
    }
    // Downloads the fill's destination pages and checks them, whole, against the shadow copy. A mismatch is
    // reported (the first few in full) and the pages take the shadow's bytes, so the run goes on from the CPU's.
    void compare(const Imx1GpuFill& fill) override {
        const uint32_t first = fill.base / pageBytes * pageBytes, end = ((fill.end() - 1) / pageBytes + 1) * pageBytes;
        cause = whole;
        cpuReads(fill.bank, first, end);
        uint32_t size = 0, shadowSize = 0;
        uint8_t* bytes = memory->rawBank(fill.bank, size);
        const uint8_t* expected = shadow->rawBank(fill.bank, shadowSize);
        ++stats.compared;
        if (!std::memcmp(bytes + first, expected + first, end - first)) return;
        uint32_t at = first;
        while (bytes[at] == expected[at]) ++at;
        at &= ~1u;
        if (++stats.mismatches <= 8)
            std::fprintf(stderr, "zodiac gpu: mismatch after fill %" PRIu64 " (bank %u offset %#x, gpu %04x cpu %04x; "
                         "fill base %#x pitch %u %ux%u colour %#x%s%s%s%s%s%s%s%s source bank %u base %#x pitch %u)\n",
                         stats.fills, fill.bank, at, unsigned(bytes[at] | bytes[at + 1] << 8),
                         unsigned(expected[at] | expected[at + 1] << 8), fill.base, fill.pitch, fill.width, fill.height,
                         fill.colour, fill.copy ? " copy" : "", fill.keyed ? " keyed" : "", fill.keyEqual ? " key-equal" : "",
                         fill.doubled ? " doubled" : "", fill.painted ? " painted" : "", fill.invert ? " invert" : "",
                         fill.a4 ? " a4" : "", fill.pixels ? " inline" : "", fill.sourceBank, fill.sourceBase,
                         fill.sourcePitch);
        std::memcpy(bytes + first, expected + first, end - first);
        cpuWrites(fill.bank, first, end);
    }
    // A fill drawn on host bytes exactly as the GPU's shader draws it, for running commands again after a failure.
    // banks: both banks' bytes; the source and the destination share no byte and no rows overlap, so in place.
    static void drawOnCpu(uint8_t* const (&banks)[2], const Imx1GpuFill& fill, const uint16_t* pixels) {
        auto at = [](const uint8_t* bank, uint32_t texel) { return uint32_t(bank[texel * 2] | bank[texel * 2 + 1] << 8); };
        auto blend = [](uint32_t d, uint32_t s, uint32_t a, uint32_t shift, uint32_t mask) {
            return ((((d >> shift) & mask) * (15u - a) + ((s >> shift) & mask) * a) / 15u) << shift;
        };
        uint8_t* to = banks[fill.bank];
        const uint8_t* source = banks[fill.sourceBank];
        const uint32_t base = fill.base / 2, pitch = fill.pitch / 2, sourceBase = fill.sourceBase / 2,
                       sourcePitch = fill.sourcePitch / 2;
        for (uint32_t y = 0; y < fill.height; ++y)
            for (uint32_t x = 0; x < fill.width; ++x) {
                const uint32_t address = base + y * pitch + x;
                uint32_t c = fill.colour;
                if (pixels) c = pixels[size_t(y) * fill.width + x];
                if (fill.copy) {
                    c = at(source, fill.doubled ? sourceBase + ((fill.sourceY + y) >> 1) * sourcePitch + ((fill.sourceX + x) >> 1) :
                                                  sourceBase + y * sourcePitch + x);
                    if (fill.keyed && ((c == fill.colour) != fill.keyEqual)) continue;
                    if (fill.painted) c = fill.paint;
                }
                if (fill.invert) c = (at(to, address) ^ c) & 65535u;
                if (fill.a4) {
                    const uint32_t d = at(to, address), s = at(source, sourceBase + y * sourcePitch + x);
                    c = c == 0u ? d : c == 15u ? s : blend(d, s, c, 0, 31) | blend(d, s, c, 5, 63) | blend(d, s, c, 11, 31);
                }
                to[address * 2] = uint8_t(c);
                to[address * 2 + 1] = uint8_t(c >> 8);
            }
    }
   private:
    // A command posted to the backend and not yet confirmed, kept to run again on the CPU if the backend breaks.
    struct Entry {
        uint64_t number;
        enum Kind { upload, fill, patch } kind;
        unsigned bank;
        uint32_t firstPage, pages;
        Imx1GraphicsGpuBackend::Bytes data;
        Imx1GpuFill drawn;
        Imx1GraphicsGpuBackend::Pixels pixels;
    };
    void logCommand(Entry entry) {
        const uint64_t done = backend->confirmed();
        while (!log.empty() && log.front().number <= done) log.pop_front();
        log.push_back(std::move(entry));
    }
    // Waits for command's confirm; false after recovering from a failure instead.
    bool confirm(uint64_t command) {
        if (backend->confirmed() >= command) return true;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = backend->waitConfirmed(command);
        stats.waitNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        ++stats.waits;
        ++stats.waitsFor[cause];
        if (!ok) recover();
        return ok;
    }
    // Copies a page's bytes from from to to except where mask is set, eight at a time where it can.
    static void merge(uint8_t* to, const uint8_t* from, const uint8_t* mask) {
        for (uint32_t b = 0; b < pageBytes; b += 8) {
            uint64_t m = 0;
            std::memcpy(&m, mask + b, 8);
            if (!m) std::memcpy(to + b, from + b, 8);
            else if (m != 0x0101010101010101ull)
                for (uint32_t i = b; i < b + 8; ++i)
                    if (!mask[i]) to[i] = from[i];
        }
    }
    void release(unsigned bank, uint32_t page) {
        std::memset(masks[size_t(slot[bank][page])].data(), 0, pageBytes);
        freeSlots.push_back(slot[bank][page]);
        slot[bank][page] = -1;
    }
    // Before the GPU uses bank offsets first to end: uploads the pages newer in the bytes and patches the CPU's
    // masked writes into GPU-newer ones.
    void prepare(unsigned bank, uint32_t first, uint32_t end) {
        if (end <= first) return;
        const uint32_t last = std::min<uint32_t>((end - 1) / pageBytes, uint32_t(pages[bank].size()) - 1);
        uint32_t size = 0;
        const uint8_t* bytes = memory->rawBank(bank, size);
        for (uint32_t page = first / pageBytes; page <= last;) {
            if (pages[bank][page] == cpuNewer) {
                uint32_t run = page;
                while (run <= last && pages[bank][run] == cpuNewer) pages[bank][run++] = same;
                auto data = std::make_shared<const std::vector<uint8_t>>(bytes + size_t(page) * pageBytes,
                                                                         bytes + size_t(run) * pageBytes);
                const uint64_t number = backend->upload(bank, page, run - page, data);
                logCommand({number, Entry::upload, bank, page, run - page, data, {}, nullptr});
                ++stats.uploads;
                stats.uploadPages += run - page;
                page = run;
            } else if (slot[bank][page] >= 0) {
                // GPU-newer pages the CPU wrote since, or pages back in the bytes that kept their CPU writes' mask.
                uint32_t run = page;
                while (run <= last && pages[bank][run] != cpuNewer && slot[bank][run] >= 0) ++run;
                const size_t span = size_t(run - page) * pageBytes;
                auto data = std::make_shared<std::vector<uint8_t>>(span * 2);
                std::memcpy(data->data(), bytes + size_t(page) * pageBytes, span);
                for (uint32_t i = page; i < run; ++i) {
                    std::memcpy(data->data() + span + size_t(i - page) * pageBytes, masks[size_t(slot[bank][i])].data(),
                                pageBytes);
                    release(bank, i);
                }
                const uint64_t number = backend->patch(bank, page, run - page, data);
                logCommand({number, Entry::patch, bank, page, run - page, data, {}, nullptr});
                for (uint32_t i = page; i < run; ++i)
                    if (pages[bank][i] == gpuNewer) need[bank][i] = number;
                ++stats.patches;
                stats.patchPages += run - page;
                page = run;
            } else ++page;
        }
    }
    // The backend broke: runs the commands since its last confirm again on the replica, brings every GPU-newer page
    // back from it under the CPU's masked writes, and keeps fills on the CPU from then on.
    void recover() {
        if (lost) return;
        lost = true;
        backend->drain();
        const uint64_t done = backend->confirmed();
        uint8_t* const banks[2] = {replica[0].data(), replica[1].data()};
        for (const auto& entry : log) {
            if (entry.number <= done) continue;
            ++stats.replayed;
            uint8_t* to = banks[entry.bank] + size_t(entry.firstPage) * pageBytes;
            const size_t span = size_t(entry.pages) * pageBytes;
            if (entry.kind == Entry::upload) std::memcpy(to, entry.data->data(), span);
            else if (entry.kind == Entry::patch) {
                const uint8_t* values = entry.data->data();
                for (size_t i = 0; i < span; ++i)
                    if (values[span + i]) to[i] = values[i];
            } else drawOnCpu(banks, entry.drawn, entry.pixels ? entry.pixels->data() : nullptr);
        }
        log.clear();
        ++stats.recoveries;
        std::fprintf(stderr, "zodiac gpu: the GPU failed after command %" PRIu64 ", %" PRIu64
                     " commands since then ran again on the CPU; fills stay on the CPU\n", done, stats.replayed);
        for (unsigned bank = 0; bank < 2; ++bank) {
            uint32_t size = 0;
            uint8_t* bytes = memory->rawBank(bank, size);
            for (uint32_t page = 0; page < pages[bank].size(); ++page) {
                const uint8_t* mask = slot[bank][page] >= 0 ? masks[size_t(slot[bank][page])].data() : nullptr;
                if (pages[bank][page] == gpuNewer) {
                    uint8_t* to = bytes + size_t(page) * pageBytes;
                    const uint8_t* from = replica[bank].data() + size_t(page) * pageBytes;
                    for (uint32_t b = 0; b < pageBytes; ++b)
                        if (!mask || !mask[b]) to[b] = from[b];
                }
                if (mask) release(bank, page);
                pages[bank][page] = cpuNewer;
            }
        }
        gpuNewerPages = 0;
    }
    static constexpr uint32_t pageBytes = Imx1GraphicsGpuBackend::pageBytes;
    std::unique_ptr<Imx1GraphicsGpuBackend> backend;
    Imx1GraphicsMemory* memory = nullptr;
    std::unique_ptr<Imx1GraphicsMemory> shadow;
    // The backend's replica of each bank (its thread writes it, see setReplica). A page is copied from it only once
    // need, the last command that changed the page, is confirmed; a later command on the page raises need first,
    // so the two threads never use the same page at once.
    std::array<std::vector<uint8_t>, 2> replica;
    std::array<std::vector<uint64_t>, 2> need;
    // Each GPU-newer page's bytes the GPU drew since the page last came back, from drawnFirst to drawnEnd.
    std::array<std::vector<uint16_t>, 2> drawnFirst, drawnEnd;
    // Pages the CPU wrote while the GPU held them newer, until the writes are patched into the GPU copy: slot
    // indexes masks, a byte per page byte (nonzero where written), or -1. A page keeps its mask when it comes back.
    std::array<std::vector<int32_t>, 2> slot;
    std::vector<std::vector<uint8_t>> masks;
    std::vector<int32_t> freeSlots;
    std::deque<Entry> log;
    bool lost = false;
    Stats stats;
};

#endif
