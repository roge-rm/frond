#ifndef FROND_IMX1_GRAPHICS_COMMANDS_H
#define FROND_IMX1_GRAPHICS_COMMANDS_H

#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>
#include <deque>
#include <utility>
#include "imx1_graphics_memory.h"

// Validated ring batches retain their register snapshot until guest-time completion.
class Imx1GraphicsCommands {
   public:
    void reset() { *this = {}; }
    uint64_t duration() const { return ticks; }
    // Register values and every queued batch with its fills and inline pixels.
    static constexpr uint32_t stateVersion = 3;
    template <class A> void state(A& a) {
        a.DoBuffer32(values.data(), values.size());
        for (auto& k : known) a.DoBool(k);
        uint64_t count = batches.size();
        a.Do64(count).Require(count <= 16384, "command batches");
        if (!a.ok()) return;
        batches.resize(size_t(count));
        for (auto& batch : batches) {
            a.Do32(batch.end).Do32(batch.words).Do32(batch.option);
            uint64_t fills = batch.fills.size();
            // A line packet is 4 words and draws up to 8192 runs, so the limit goes with the batch's words.
            a.Do64(fills).Require(fills <= 16384 || fills <= uint64_t(batch.words) * 2048, "command fills");
            if (!a.ok()) return;
            batch.fills.resize(size_t(fills));
            for (auto& f : batch.fills) {
                a.Do32(f.base).Do32(f.pitch).Do32(f.width).Do32(f.height).Do32(f.colour).Do32(f.sourceX).Do32(f.sourceY);
                a.Do32(f.sourceBase).Do32(f.sourcePitch).DoBool(f.keyed).DoBool(f.plain).DoBool(f.a4).DoBool(f.invert);
                a.Do32(f.paint).DoBool(f.doubled).DoBool(f.keyEqual).DoBool(f.painted);
                a.DoVector(f.pixels, 1 << 20);
                a.Require(f.pixels.empty() || f.pixels.size() >= uint64_t(f.width) * f.height, "command pixels");
                if (!a.ok()) return;
            }
        }
        a.Do32(queuedWords).Do64(ticks);
    }
    Imx1BootstrapAccess prepare(const Imx1Cs5Configuration& configuration,
                                const Imx1Eim& eim, Imx1GraphicsMemory& memory) {
        // Candidate decoding owns its inline pixels and publishes nothing on rejection.
        auto candidate = *this;
        candidate.refused = {};
        const auto status = candidate.decode(configuration, eim, memory);
        if (status == Imx1BootstrapAccess::ok) *this = std::move(candidate);
        else refused = candidate.refused;
        return status;
    }
    // prepare for a copy the caller drops on refusal, reading only refusal() from it then: decodes in place, with
    // the same result as prepare where it succeeds.
    Imx1BootstrapAccess prepareCopy(const Imx1Cs5Configuration& configuration,
                                    const Imx1Eim& eim, Imx1GraphicsMemory& memory) {
        refused = {};
        return decode(configuration, eim, memory);
    }
    // What the decoder named in the batch it last refused, with one value. Not saved.
    struct Refusal { const char* what = nullptr; uint32_t value = 0; };
    const Refusal& refusal() const { return refused; }
   private:
    Imx1BootstrapAccess decode(const Imx1Cs5Configuration& configuration,
                               const Imx1Eim& eim, Imx1GraphicsMemory& memory) {
        using A = Imx1BootstrapAccess;
        const auto& engine = configuration.engineState();
        const uint32_t words = engine.ringBytes() / 4;
        if (!words || batches.size() >= words - 1) return A::sequence;
        uint32_t cursor = engine.submissionBegin();
        if (cursor != (batches.empty() ? engine.consumer() : batches.back().end)) return A::sequence;
        uint32_t left = (engine.producer() - cursor) & (words - 1);
        if (!left) { ticks = 1; return A::ok; }
        if (left > words - 1 - queuedWords) return A::sequence;
        Batch batch{};
        batch.end = engine.producer();
        batch.words = left;
        auto& fills = batch.fills;
        ticks = left + 1;
        auto take = [&](uint32_t& word) {
            if (!left) return false;
            if (memory.accessGpu(configuration, eim, engine.ringAddress() + cursor * 4, 4, false, word) != A::ok) return false;
            cursor = (cursor + 1) & (words - 1);
            --left;
            return true;
        };
        // Without the policy every packed 1bpp path below is inert.
        const bool packedMono = configuration.packedMonoEnabled();
        uint32_t header, hostPackets = 0, copyPackets = 0, plainPackets = 0, a4Packets = 0, otherPackets = 0;
        const auto other = engine.otherSubmission();
        // 410 to 412, 422 and 464 to 466 come only with the stretch blit.
        // 464 to 466 come with the rotate and mirror blit too.
        auto put = [&](uint32_t reg, uint32_t value) {
            if (other == Imx1GraphicsEngine::transform && reg >= 0x464 && reg <= 0x466) return store(reg, value, packedMono);
            if (other != Imx1GraphicsEngine::stretchBlit && ((reg >= 0x410 && reg <= 0x412) || reg == 0x422 ||
                                                             (reg >= 0x464 && reg <= 0x466))) return false;
            return store(reg, value, packedMono);
        };
        while (left) {
            if (!take(header)) return A::sequence;
            uint32_t first, second;
            if ((header >> 30) == 0) {
                // (count - 1) << 16 | first register, then a value for each register in order. Only the
                // stretch blit sends more than one, in threes (200b0a10-200b0acc).
                const uint32_t count = ((header >> 16) & 0x3fff) + 1, reg = header & 0x7ff;
                if ((header & 0xf800u) || (count != 1 && (count != 3 || (reg != 0x410 && reg != 0x420 && reg != 0x464))))
                    return A::sideEffect;
                for (uint32_t i = 0; i < count; ++i)
                    if (!take(first) || !put(reg + i, first)) return A::sideEffect;
                if (reg + count - 1 == 0x466) {
                    // The write of 466 starts the stretch, or the rotate and mirror blit.
                    if (count != 3) return A::sideEffect;
                    const bool turned = other == Imx1GraphicsEngine::transform;
                    if (!fills.empty() || ++otherPackets != 1) return refuse(turned ? "transform-batch" : "stretch-batch", reg);
                    Fill fill{};
                    const auto status = turned ? transformBlit(configuration, eim, memory, fill) :
                                                 stretchBlit(configuration, eim, memory, fill);
                    if (status != A::ok) return status;
                    ticks += uint64_t(fill.width) * fill.height;
                    fills.push_back(std::move(fill));
                }
            } else if ((header >> 30) == 1) {
                if (header & 0x3fc00000u || !take(first) || !take(second) ||
                    !put(header & 0x7ff, first) || !put((header >> 11) & 0x7ff, second)) return A::sideEffect;
            } else if ((header & 0xc000ffffu) == 0xc0001500u) {
                // The line routine (200afd00, from 2008ae14) clips each segment to the surface's clip rectangle
                // in software (200afba8) and sends it as its own packet: start, end and the end again
                // (200aff24-200affa0), each point x | y << 16.
                if (other != Imx1GraphicsEngine::line || header != 0xc0021500) return refuse("line-packet", header);
                otherPackets = 1;
                uint32_t points[3];
                for (auto& point : points) if (!take(point)) return A::sequence;
                if (points[2] != points[1]) return refuse("line-points", points[2]);
                const auto status = line(configuration, eim, memory, points[0], points[1], fills);
                if (status != A::ok) return status;
            } else if (other == Imx1GraphicsEngine::blend) {
                // TwGfxBlendBlt (200a9414 with flag 1, from 2009acc8): source 2 (418, 419, origin 416 as x << 16 | y,
                // size 420/421) blended over the destination rectangle of one paint packet at the global alpha (484).
                if (header != 0xc0011a00) return refuse("blend-packet", header);
                if (!fills.empty() || ++otherPackets != 1) return refuse("blend-batch", header);
                if (!take(first) || !take(second)) return A::sequence;
                Fill fill{};
                const auto status = rectangle(configuration, eim, memory, first, second, fill, false, false, false, false, false, true);
                if (status != A::ok) return status;
                const auto sourceStatus = blendSource(configuration, eim, memory, second, fill);
                if (sourceStatus != A::ok) return sourceStatus;
                ticks += uint64_t(fill.width) * fill.height;
                fills.push_back(std::move(fill));
            } else if (other == Imx1GraphicsEngine::monoBrush || other == Imx1GraphicsEngine::colourBrush) {
                // The brush upload of 200aedd0: a one row rectangle on the brush surface (423), then the
                // pattern as host words through 4f0. They land in memory as sent: 8 words with a pattern
                // row in each low byte (200af0d0-200af1fc), 4 words for a 32 by 1 brush, or 64 pixels
                // of 16 bits in 32 words (200af3a0-200af448). The scissor is off (41b bits 2 and 3 clear).
                const bool colour = other == Imx1GraphicsEngine::colourBrush;
                if (header != 0xc0011a00) return refuse("brush-header", header);
                if (!fills.empty() || ++otherPackets != 1) return refuse("brush-batch", header);
                uint32_t data;
                if (!take(first) || !take(second) || !take(data)) return A::sequence;
                for (auto reg : {0x4a1,0x41b,0x472,0x4b2,0x4b1,0x401,0x402,0x488,0x423,0x4a0}) if (!known[reg]) return A::sequence;
                const uint32_t words = colour ? 32 : (second >> 16) == 8 ? 4 : 8;
                if (first || second != ((words * 2) << 16 | 1) || data != (colour ? 0x001f84f0u : ((words - 1) << 16 | 0x4f0)))
                    return refuse("brush-packet", second);
                for (auto item : {std::pair<uint32_t,uint32_t>{0x41b, colour ? 0x5798b4f3u : 0x5798b303u},
                         {0x4b2, 0x00cc0b00}, {0x4b1, colour ? 0x40036f04u : 0x40036003u}, {0x402, words * 2},
                         {0x401, values[0x423]}, {0x472, 0x3f}, {0x488, 4}, {0x4a1, 0}, {0x4a0, 0}})
                    if (values[item.first] != item.second) return refuse("brush-state", item.first);
                if (left != words) return left < words ? A::sequence : A::sideEffect;
                Fill fill{};
                fill.base = values[0x401];
                fill.pitch = words * 4;
                fill.width = words * 2;
                fill.height = 1;
                if ((fill.base & 3) || !memory.contains(configuration, eim, fill.base, words * 4)) return A::sequence;
                for (uint32_t word = 0; word < words; ++word) {
                    if (!take(data)) return A::sequence;
                    fill.pixels.push_back(uint16_t(data));
                    fill.pixels.push_back(uint16_t(data >> 16));
                }
                ticks += fill.width;
                fills.push_back(std::move(fill));
            } else if ((header & 0xc000ffffu) == 0xc0001a00u) {
                // One packet holds n rectangles as 2n words. Only the fill routine (200b0038) sends more
                // than one: the strips of a frame, four at most (2008aae0).
                const uint32_t count = ((header >> 16) & 0x3fff) + 1;
                if (engine.hostDataSubmission() || engine.keyedCopySubmission() || engine.a4Submission() || other ||
                    engine.stretchCopySubmission() || (count & 1))
                    return A::sideEffect;
                if (count > (engine.fillSubmission() ? 8u : 2u)) return refuse("rectangle-count", count / 2);
                // Brush types 0, 1, 6, 7 and a draw the pattern uploaded to 423. Where the chip anchors it
                // is not known and no caller in the ROM passes an origin (41d), so they are not drawn.
                if (known[0x41b] && ((values[0x41b] >> 4) & 15) != 13) return refuse("pattern-brush", (values[0x41b] >> 4) & 15);
                for (uint32_t pair = 0; pair < count; pair += 2) {
                    if (!take(first) || !take(second)) return A::sequence;
                    Fill fill{};
                    const auto status = rectangle(configuration, eim, memory, first, second, fill, false);
                    if (status != A::ok) return status;
                    // ROP3 5a is destination xor pattern, f0 is the pattern alone.
                    fill.invert = ((values[0x41b] >> 17) & 255) == 0x5a;
                    if (fill.invert && !engine.fillSubmission()) return A::sideEffect;
                    ticks += uint64_t(fill.width) * fill.height;
                    fills.push_back(std::move(fill));
                }
            } else if (header == 0xc0051c00 || (header & 0xc000ffffu) == 0xc0001b00u) {
                const bool plain = header != 0xc0051c00;
                // A plain copy packet holds n copies as 3n words. The copy helper (200a9cf8) sends one,
                // the software stretch (200b0644) one for each run of its x and y tables.
                const uint32_t copyWords = plain ? ((header >> 16) & 0x3fff) + 1 : 3;
                const bool stretch = engine.stretchCopySubmission();
                if (copyWords % 3 || (copyWords != 3 && !stretch)) return A::sideEffect;
                if (plain ? ((!engine.copyHelperSubmission() && !stretch) || !fills.empty() || ++plainPackets != 1) :
                            (!engine.keyedCopySubmission() || ++copyPackets != 1)) return A::sideEffect;
                uint32_t sourceKey = 0, sourceXY;
                bool keyEqual = false;
                if (!plain) {
                    uint32_t control, destinationKey;
                    if (!take(control) || !take(sourceKey) || !take(destinationKey)) return A::sequence;
                    // Source key alone: 5 for mask and overlay, 4 for erase (200aa30c-200aa374, 2008a870).
                    if (control != 0x01000005 && control != 0x01000004) return refuse("keyed-control", control);
                    if (sourceKey > 0xffff || destinationKey) return A::sideEffect;
                    keyEqual = control == 0x01000004;
                }
                std::vector<Area> sources, destinations;
                std::vector<bool> covered;
                for (uint32_t copy = 0; copy < copyWords; copy += 3) {
                    if (!take(sourceXY) || !take(first) || !take(second)) return A::sequence;
                    // Both copy producers send 48f with every copy and never 490 or 491. A colour source
                    // copy reads neither.
                    if ((copy + 3 == copyWords && left) || !known[0x48f] || values[0x48f] != 0xffffffff || !staleTextColours()) return A::sideEffect;
                    // The software stretch sends a run of no width or height where a shrink drops a column or
                    // row (seen in Outpost). It doesn't filter them, so on the chip they draw nothing.
                    if (stretch && (!(second >> 16) || !(second & 0xffff))) continue;
                    Fill fill{};
                    const auto status = rectangle(configuration, eim, memory, first, second, fill, false, true, false, plain);
                    if (status != A::ok) return status;
                    const uint32_t x = sourceXY >> 16, y = sourceXY & 0xffff;
                    const uint32_t width = second >> 16, height = second & 0xffff;
                    const uint32_t pitch = values[0x46c];
                    if (!pitch || pitch > 8192 || x + width > pitch ||
                        x + width > 8191 || y + height > 8191)
                        return A::sideEffect;
                    const uint64_t address = uint64_t(values[0x46b]) + (uint64_t(y) * pitch + x) * 2;
                    const uint64_t length = (uint64_t(height - 1) * pitch + width) * 2;
                    if (address > 0xffffffffu || (address & 1) ||
                        !memory.contains(configuration, eim, uint32_t(address), length)) return A::sequence;
                    fill.sourcePitch = pitch * 2;
                    fill.sourceBase = uint32_t(address + uint64_t(fill.sourceY) * fill.sourcePitch + fill.sourceX * 2);
                    fill.keyed = !plain;
                    fill.plain = plain;
                    // With ROP f0 a keyed copy paints the solid brush colour through the key (mask and erase,
                    // set up by 2009c4ac). ASSUMPTION "keyed copy compare 4": 4 draws where the source equals
                    // the key. Compare 5 draws where it differs, as the overlay pictures show, and the driver
                    // sends 4 for erase alone, which paints where the source is transparent.
                    fill.painted = !plain && values[0x41b] == 0x45e0b4df;
                    fill.paint = fill.colour;
                    fill.keyEqual = keyEqual;
                    fill.colour = sourceKey;
                    // Overlapping pixels need direction rules that this producer has not qualified. Rectangles
                    // side by side on one surface share no byte and are drawn.
                    const Area from{address, uint64_t(pitch) * 2, uint64_t(width) * 2, height};
                    const Area to{fill.base, fill.pitch, uint64_t(fill.width) * 2, fill.height};
                    if (from.overlaps(to)) return refuse("copy-overlap", copy / 3);
                    if (copyWords != 3) {
                        // The stretch never sends two runs to the same place (its tables partition the
                        // destination), and what the chip does with an overlap is not known. Refused.
                        const uint32_t dx = first >> 16, dy = first & 0xffff;
                        if (dx + width > stretchLimit || dy + height > stretchLimit) return refuse("stretch-run-bounds", copy / 3);
                        if (covered.empty()) covered.resize(size_t(stretchLimit) * stretchLimit);
                        for (uint32_t row = dy; row < dy + height; ++row)
                            for (uint32_t column = dx; column < dx + width; ++column) {
                                if (covered[size_t(row) * stretchLimit + column]) return refuse("stretch-runs-overlap", copy / 3);
                                covered[size_t(row) * stretchLimit + column] = true;
                            }
                        sources.push_back(from);
                        destinations.push_back(to);
                    }
                    ticks += uint64_t(fill.width) * fill.height;
                    fills.push_back(std::move(fill));
                }
                // With the sources clear of every destination the order of the copies changes nothing.
                for (size_t i = 0; i < sources.size(); ++i)
                    for (size_t j = 0; j < destinations.size(); ++j)
                        if (sources[i].overlaps(destinations[j])) return refuse("stretch-source-overlap", uint32_t(i));
            } else if ((header & 0xc000ffffu) == 0xc0001900u) {
                const bool a4 = engine.a4Submission();
                // The host producer writes source field 0 into GMC bits 12..14 for packed 1bpp.
                const bool mono = packedMono && !a4 && known[0x41b] && !((values[0x41b] >> 12) & 7);
                if (a4 ? (!configuration.a4SourceOverEnabled() || ++a4Packets != 1) :
                         (!engine.hostDataSubmission() || ++hostPackets != 1)) return A::sideEffect;
                if (!take(first) || !take(second)) return A::sequence;
                const uint32_t width = second & 0xffff, height = second >> 16;
                // A4 is linear, eight low-nibble-first samples per word, without row padding.
                // Packed 1bpp rows are padded to 32 pixels, one word per 32.
                const uint64_t payload = mono ? uint64_t(width / 32) * height :
                    a4 ? (uint64_t(width) * height + 7) / 8 : uint64_t(width / 2) * height;
                // A4 width and height may be odd: the ROM's word count is the linear one, (w * h + 7) / 8 words
                // (200a9888-200a98a0; 4002 for 252x127 at 200a9a9c), so rows don't start on a byte and the
                // last word is part used.
                if (!width || (width & (mono ? 31 : a4 ? 0 : 1)) || !height ||
                    payload + 1 != ((header >> 16) & 0x3fff))
                    return A::sideEffect;
                if (payload > left) return A::sequence;
                if (payload != left) return A::sideEffect;
                Fill fill{};
                const auto status = rectangle(configuration, eim, memory, first, second, fill, true, false, a4, false, mono);
                if (status != A::ok) return status;
                if (a4) {
                    const auto sourceStatus = a4Source(configuration, eim, memory, first, width, height, fill);
                    if (sourceStatus != A::ok) return sourceStatus;
                }
                fill.pixels.reserve(size_t(fill.width) * fill.height);
                const uint32_t samplesPerWord = mono ? 32 : a4 ? 8 : 2;
                for (uint64_t word = 0; word < payload; ++word) {
                    uint32_t pixels;
                    if (!take(pixels)) return A::sequence;
                    for (uint32_t sample = 0; sample < samplesPerWord; ++sample) {
                        const uint64_t index = word * samplesPerWord + sample;
                        if (index >= uint64_t(width) * height) break;
                        const uint32_t y = index / width, x = index % width;
                        if (y >= fill.sourceY && y - fill.sourceY < fill.height &&
                            x >= fill.sourceX && x - fill.sourceX < fill.width)
                            fill.pixels.push_back(mono ? virtualPackedMonoColour(virtualPackedMonoBit(pixels, sample)) :
                                uint16_t((pixels >> (sample * (a4 ? 4 : 16))) & (a4 ? 15 : 0xffff)));
                    }
                }
                ticks += uint64_t(fill.width) * fill.height;
                fills.push_back(std::move(fill));
            } else return A::sideEffect;
        }
        if ((engine.hostDataSubmission() && hostPackets != 1) ||
            (engine.keyedCopySubmission() && copyPackets != 1) ||
            (engine.a4Submission() && a4Packets != 1) || (other && otherPackets != 1) ||
            (values[0x41b] == 0x5598b4df && plainPackets != 1 && other != Imx1GraphicsEngine::stretchBlit &&
             other != Imx1GraphicsEngine::transform)) return A::sideEffect;
        batch.option = values[0x4a0];
        queuedWords += batch.words;
        batches.push_back(std::move(batch));
        return A::ok;
    }
   public:
    // Tests: directFills off makes complete take every fill through accessGpu; directFillCount counts the fills
    // fastFill took.
    static inline bool directFills = true;
    static inline uint64_t directFillCount = 0;
    Imx1BootstrapAccess complete(Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                 Imx1GraphicsMemory& memory) {
        using A = Imx1BootstrapAccess;
        if (!configuration.engineState().completed()) return A::sequence;
        while (configuration.engineState().completed()) {
            if (batches.empty()) return A::sequence;
            const auto& batch = batches.front();
            // Validate both surfaces before writing this batch, including after reset.
            for (const auto& fill : batch.fills) {
                const uint64_t destinationLength = fill.height ? uint64_t(fill.height - 1) * fill.pitch + fill.width * 2 : 0;
                const uint64_t sourceLength = !fill.height ? 0 : sourceBytes(fill);
                if (destinationLength && ((fill.base & 1) || !memory.contains(configuration, eim, fill.base, destinationLength) ||
                    ((fill.keyed || fill.plain || fill.a4) && !memory.contains(configuration, eim, sourceStart(fill), sourceLength)))) return A::sequence;
            }
            for (const auto& fill : batch.fills) {
                const auto status = completeFill(configuration, eim, memory, fill);
                if (status != A::ok) return status;
            }
            configuration.completeEngine(batch.option);
            queuedWords -= batch.words;
            batches.pop_front();
        }
        return A::ok;
    }
    struct Fill {
        uint32_t base = 0, pitch = 0, width = 0, height = 0, colour = 0, sourceX = 0, sourceY = 0;
        uint32_t sourceBase = 0, sourcePitch = 0;
        bool keyed = false, plain = false, a4 = false, invert = false;
        // An A4 fill without pixels is the constant blend, its alphas in colour as 484 holds them.
        // A plain fill with keyEqual is the rotate and mirror blit: its source moves by colour bytes for each
        // destination column and by sourcePitch bytes for each row, either of them negative (two's complement).
        // paint: the colour a keyed copy with ROP f0 writes. doubled: a stretch blit source, read at the ratios
        // in paint (8.8, x low, y high; 0 for 80 hex both ways, each pixel twice, as a fill saved before ratios).
        uint32_t paint = 0;
        bool doubled = false, keyEqual = false, painted = false;
        std::vector<uint16_t> pixels;
    };
    // One fill of a batch complete has checked: straight on the bank storage where fastFill can take it, else
    // through accessGpu. Public for tests.
    // With a host GPU attached (Fast only) the fill goes there where gpuFill can take it; in differential mode it
    // runs on the GPU's CPU-only shadow copy too and the GPU's pages are checked against it.
    static Imx1BootstrapAccess completeFill(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                            Imx1GraphicsMemory& memory, const Fill& fill) {
        if (auto* gpu = memory.gpuResidency()) {
            Imx1GraphicsMemory* shadow = gpu->mirror ? gpu->shadowMemory() : nullptr;
            if (shadow) completeFillCpu(configuration, eim, *shadow, fill);
            Imx1GpuFill drawn;
            if (gpuFill(configuration, eim, memory, fill, drawn) && gpu->fill(drawn)) {
                if (shadow) gpu->compare(drawn);
                return Imx1BootstrapAccess::ok;
            }
            ++gpu->cpuFills;
        }
        return completeFillCpu(configuration, eim, memory, fill);
    }
    // Tests: gpuFills off keeps every fill on the CPU.
    static inline bool gpuFills = true;
    // A fill for the host GPU, with both ranges resolved to bank offsets. Only fills the GPU draws exactly as the
    // CPU loop does: rows that don't overlap each other, and a source that shares no byte with the destination
    // (the CPU loop reads and writes pixel by pixel, the GPU reads the source as it was before the fill).
    static bool gpuFill(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim, const Imx1GraphicsMemory& memory,
                        const Fill& fill, Imx1GpuFill& out) {
        if (!gpuFills || !fill.width || !fill.height || ((fill.base | fill.pitch) & 1) || (fill.a4 && fill.doubled) ||
            (fill.a4 && fill.pixels.empty()) || transformed(fill) ||
            (fill.doubled && (ratioX(fill) != 0x80 || ratioY(fill) != 0x80)) ||
            (fill.height > 1 && fill.pitch < fill.width * 2) ||
            (!fill.pixels.empty() && fill.pixels.size() != size_t(fill.width) * fill.height)) return false;
        const bool copy = fill.keyed || fill.plain, source = copy || fill.a4;
        if (source && ((fill.sourceBase | fill.sourcePitch) & 1)) return false;
        const uint64_t length = uint64_t(fill.height - 1) * fill.pitch + fill.width * 2;
        if (!memory.contains(configuration, eim, fill.base, length, &out.bank, &out.base)) return false;
        out.pitch = fill.pitch;
        out.width = fill.width;
        out.height = fill.height;
        out.colour = fill.colour;
        out.copy = copy;
        out.keyed = fill.keyed;
        out.doubled = copy && fill.doubled;
        out.invert = fill.invert;
        out.a4 = fill.a4;
        out.painted = copy && fill.painted;
        out.keyEqual = fill.keyEqual;
        out.paint = fill.paint;
        out.pixels = !copy && !fill.pixels.empty() ? fill.pixels.data() : nullptr;
        if (source) {
            const uint64_t sourceLength = sourceBytes(fill);
            if (!memory.contains(configuration, eim, fill.sourceBase, sourceLength, &out.sourceBank, &out.sourceBase)) return false;
            if (out.sourceBank == out.bank && out.sourceBase < out.base + length && out.base < out.sourceBase + sourceLength)
                return false;
            out.sourcePitch = fill.sourcePitch;
            out.sourceLength = uint32_t(sourceLength);
            out.sourceX = fill.doubled ? fill.sourceX : 0;
            out.sourceY = fill.doubled ? fill.sourceY : 0;
        }
        return true;
    }
    // The CPU model's fill. Public for tests.
    static Imx1BootstrapAccess completeFillCpu(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                               Imx1GraphicsMemory& memory, const Fill& fill) {
        using A = Imx1BootstrapAccess;
        if (fastFill(configuration, eim, memory, fill)) return A::ok;
        for (uint32_t y = 0; y < fill.height; ++y)
            for (uint32_t x = 0; x < fill.width; ++x) {
                uint32_t colour = fill.pixels.empty() ? fill.colour : fill.pixels[size_t(y) * fill.width + x];
                if (fill.keyed || fill.plain) {
                    // A stretch source steps from the unclipped origin at the ratios.
                    const uint32_t at = fill.doubled ?
                        fill.sourceBase + stretchStep(fill.sourceY + y, ratioY(fill)) * fill.sourcePitch +
                        stretchStep(fill.sourceX + x, ratioX(fill)) * 2 :
                        transformed(fill) ? fill.sourceBase + y * fill.sourcePitch + x * fill.colour :
                        fill.sourceBase + y * fill.sourcePitch + x * 2;
                    const auto status = memory.accessGpu(configuration, eim, at, 2, false, colour);
                    if (status != A::ok) return status;
                    if (fill.keyed && (colour == fill.colour) != fill.keyEqual) continue;
                    if (fill.painted) colour = fill.paint;
                }
                if (fill.invert) {
                    uint32_t destination = 0;
                    if (memory.accessGpu(configuration, eim, fill.base + y * fill.pitch + x * 2,
                                         2, false, destination) != A::ok) return A::sequence;
                    colour = (destination ^ colour) & 0xffff;
                }
                if (fill.a4) {
                    uint32_t source = 0, destination = 0;
                    if (memory.accessGpu(configuration, eim, fill.sourceBase + y * fill.sourcePitch + x * 2,
                                         2, false, source) != A::ok ||
                        memory.accessGpu(configuration, eim, fill.base + y * fill.pitch + x * 2,
                                         2, false, destination) != A::ok) return A::sequence;
                    colour = fill.pixels.empty() ? virtualBlend(destination, source, fill.colour) :
                                                   virtualA4Blend(destination, source, colour);
                }
                const auto status = memory.accessGpu(configuration, eim, fill.base + y * fill.pitch + x * 2,
                                                      2, true, colour);
                if (status != A::ok) return status;
            }
        return A::ok;
    }
   private:
    static uint32_t read16(const uint8_t* at) { return uint32_t(at[0]) | uint32_t(at[1]) << 8; }
    // fastFill's pixels for one kind of fill, picked at compile time: inline pixels, a source copy (keyed or
    // not, doubled or not), invert and A4. Each pixel reads and writes in the accessGpu loop's order, so a source
    // or row that overlaps the destination gives the same result. first and end take each row's written span.
    template <bool kPixels, bool kCopy, bool kKeyed, bool kDoubled, bool kInvert, bool kA4>
    static void fillRows(const Fill& fill, uint8_t* destination, const uint8_t* from, uint32_t offset,
                         uint64_t& writes, uint32_t& first, uint32_t& end) {
        for (uint32_t y = 0; y < fill.height; ++y) {
            const uint32_t row = y * fill.pitch;
            uint8_t* out = destination + row;
            const uint16_t* in = kPixels ? fill.pixels.data() + size_t(y) * fill.width : nullptr;
            const uint8_t* source = !kCopy ? nullptr :
                from + (kDoubled ? stretchStep(fill.sourceY + y, ratioY(fill)) * fill.sourcePitch : y * fill.sourcePitch);
            const uint8_t* alphaSource = kA4 ? from + y * fill.sourcePitch : nullptr;
            uint32_t low = 0, high = 0;
            for (uint32_t x = 0; x < fill.width; ++x) {
                uint32_t colour = kPixels ? in[x] : fill.colour;
                if constexpr (kCopy) {
                    colour = read16(source + (kDoubled ? stretchStep(fill.sourceX + x, ratioX(fill)) * 2 : x * 2));
                    if (kKeyed && (colour == fill.colour) != fill.keyEqual) continue;
                    if (fill.painted) colour = fill.paint;
                }
                if constexpr (kInvert) colour = (read16(out + x * 2) ^ colour) & 0xffff;
                if constexpr (kA4) {
                    // Alpha 0 keeps the destination and 15 takes the source, as virtualA4Blend gives.
                    const uint32_t under = read16(out + x * 2), over = read16(alphaSource + x * 2);
                    if (!fill.pixels.empty()) colour = colour == 0 ? under : colour == 15 ? over : virtualA4Blend(under, over, colour);
                    else colour = virtualBlend(under, over, fill.colour);
                }
                out[x * 2] = uint8_t(colour);
                out[x * 2 + 1] = uint8_t(colour >> 8);
                if constexpr (kKeyed) {
                    if (high == 0) low = x * 2;
                    high = x * 2 + 2;
                    ++writes;
                }
            }
            if constexpr (!kKeyed) {
                high = fill.width * 2;
                writes += fill.width;
            }
            if (high) {
                first = std::min(first, offset + row + low);
                end = std::max(end, offset + row + high);
            }
        }
    }
    template <bool... kFixed>
    static void fillKind(const bool* kinds, const Fill& fill, uint8_t* destination, const uint8_t* from,
                         uint32_t offset, uint64_t& writes, uint32_t& first, uint32_t& end) {
        if constexpr (sizeof...(kFixed) == 6) fillRows<kFixed...>(fill, destination, from, offset, writes, first, end);
        else if (kinds[sizeof...(kFixed)]) fillKind<kFixed..., true>(kinds, fill, destination, from, offset, writes, first, end);
        else fillKind<kFixed..., false>(kinds, fill, destination, from, offset, writes, first, end);
    }
    // A fill complete runs straight on the bank storage, with the same reads and writes in the same order as the
    // accessGpu loop. Only for fills where every one of those accesses is aligned and inside the ranges the batch
    // checked, so none can fail; false, with nothing done, for the rest.
    static bool fastFill(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim, Imx1GraphicsMemory& memory,
                         const Fill& fill) {
        if (!directFills || !fill.width || !fill.height || ((fill.base | fill.pitch) & 1) || (fill.a4 && fill.doubled) ||
            transformed(fill) ||
            (!fill.pixels.empty() && fill.pixels.size() != size_t(fill.width) * fill.height)) return false;
        const bool source = fill.keyed || fill.plain || fill.a4;
        if (source && ((fill.sourceBase | fill.sourcePitch) & 1)) return false;
        unsigned bank = 0, sourceBank = 0;
        uint32_t offset = 0, sourceOffset = 0;
        uint8_t* destination = memory.gpuSpan(configuration, eim, fill.base,
                                              uint64_t(fill.height - 1) * fill.pitch + fill.width * 2, bank, offset);
        if (!destination) return false;
        const uint8_t* from = nullptr;
        if (source) {
            // The source range the batch check worked out.
            const uint64_t length = sourceBytes(fill);
            from = memory.gpuSpan(configuration, eim, fill.sourceBase, length, sourceBank, sourceOffset);
            if (!from) return false;
        }
        uint64_t writes = 0;
        uint32_t first = UINT32_MAX, end = 0;
        const bool copy = fill.keyed || fill.plain;
        const bool kinds[6] = {!copy && !fill.pixels.empty(), copy, fill.keyed, copy && fill.doubled, fill.invert, fill.a4};
        fillKind<>(kinds, fill, destination, from, offset, writes, first, end);
        memory.noteWrites(bank, first, end, writes);
        ++directFillCount;
        return true;
    }
    // The software stretch takes at most 400 hex pixels each way (200b0c10).
    static constexpr uint32_t stretchLimit = 0x400;
    // A rectangle in memory: rows of bytes from base, pitch apart.
    struct Area {
        uint64_t base, pitch, bytes, rows;
        uint64_t end() const { return base + (rows - 1) * pitch + bytes; }
        // Whether a row of this one shares a byte with a row of the other.
        bool overlaps(const Area& other) const {
            if (!rows || !bytes || !other.rows || !other.bytes) return false;
            if (base >= other.end() || other.base >= end()) return false;
            if ((rows > 1 && !pitch) || (other.rows > 1 && !other.pitch)) return true;
            for (uint64_t row = 0; row < rows; ++row) {
                const uint64_t first = base + row * pitch, last = first + bytes;
                if (first >= other.end()) break;
                if (last <= other.base) continue;
                // The other's rows that start before this row ends and end after it starts.
                const uint64_t high = other.rows == 1 ? 0 : std::min(other.rows - 1, (last - other.base - 1) / other.pitch);
                const uint64_t low = first < other.base + other.bytes ? 0 :
                    other.rows == 1 ? 1 : (first - other.base - other.bytes) / other.pitch + 1;
                if (low <= high) return true;
            }
            return false;
        }
    };
    // A stretch fill's x and y ratios in 8.8.
    static uint32_t ratioX(const Fill& fill) { return fill.paint ? fill.paint & 0xffff : 0x80; }
    static uint32_t ratioY(const Fill& fill) { return fill.paint ? fill.paint >> 16 : 0x80; }
    // The source column or row a stretch reads for destination column or row i of the unclipped rectangle.
    static uint32_t stretchStep(uint32_t i, uint32_t ratio) { return uint32_t((uint64_t(i) * ratio) >> 8); }
    static bool transformed(const Fill& fill) { return fill.plain && fill.keyEqual; }
    // A transform's source step for one destination column (colour) or row (sourcePitch), signed.
    static int64_t signedStep(uint32_t step) { return int64_t(int32_t(step)); }
    // Where the bytes a fill reads begin: sourceBase, or for a transform the lowest pixel it reaches.
    static uint32_t sourceStart(const Fill& fill) {
        if (!transformed(fill) || !fill.width || !fill.height) return fill.sourceBase;
        const int64_t x = signedStep(fill.colour) * (fill.width - 1), y = signedStep(fill.sourcePitch) * (fill.height - 1);
        return uint32_t(int64_t(fill.sourceBase) + std::min<int64_t>(x, 0) + std::min<int64_t>(y, 0));
    }
    // The bytes from sourceStart a fill reads. A stretch's sourceBase is the unclipped origin's pixel.
    static uint64_t sourceBytes(const Fill& fill) {
        if (transformed(fill)) return uint64_t(std::abs(signedStep(fill.colour)) * (fill.width - 1) +
                                               std::abs(signedStep(fill.sourcePitch)) * (fill.height - 1) + 2);
        if (!fill.doubled) return uint64_t(fill.height - 1) * fill.sourcePitch + fill.width * 2;
        return uint64_t(stretchStep(fill.sourceY + fill.height - 1, ratioY(fill))) * fill.sourcePitch +
               (stretchStep(fill.sourceX + fill.width - 1, ratioX(fill)) + 1) * 2;
    }
    struct Batch {
        uint32_t end = 0, words = 0, option = 0;
        std::vector<Fill> fills;
    };
    static constexpr std::array<uint32_t,42> registers{{0x410,0x411,0x412,0x422,0x464,0x465,0x466,0x4a1,0x41b,0x41f,0x472,0x4b2,0x4b1,
        0x490,0x491,0x46f,0x470,0x401,0x402,0x46b,0x46c,0x41e,0x488,0x484,0x471,
        0x423,0x41d,0x4b3,0x30f,0x42e,0x42f,0x430,0x431,0x432,0x433,0x4a0,0x48f,0x418,0x419,0x420,0x421,0x416}};
    bool store(uint32_t reg, uint32_t value, bool packedMono) {
        if (std::find(registers.begin(),registers.end(),reg) == registers.end()) return false;
        values[reg] = value;
        known[reg] = true;
        // GMC selects the same source and ROP fields as DP_MIX in the native helpers.
        if (reg == 0x41b && known[0x4b2] &&
            (mixAlias(value) || (packedMono && value == 0x579804df)))
            values[0x4b2] = (values[0x4b2] & ~0x01ff0700u) |
                ((value >> 1) & 0x00ff0000u) | ((value >> 17) & 0x700u) | ((value >> 7) & 0x01000000u);
        return true;
    }
    // GMC words whose source and ROP fields store() copies into DP_MIX, so 4b2 holds what the engine uses after
    // any of them, whichever of 41b and 4b2 came last. d598b4df is the stretch blit's own (200b085c-200b0868).
    static bool mixAlias(uint32_t value) {
        return value == 0x5798b4df || value == 0x4598b4df || value == 0x45e0b4df || value == 0x55e0b4df ||
               value == 0xd798d4df || value == 0x5598b4df || value == 0xd598b4df;
    }
    // Packed 1bpp drawing below is explicit virtual policy. The original writer copies
    // source halfwords unchanged and proves none of it, and physical W4200 behaviour is unknown.
    // virtualPackedMonoMsbFirst: source bytes run left to right, bit 7 of each byte first.
    static bool virtualPackedMonoBit(uint32_t word, uint32_t sample) {
        return (word >> ((sample & ~7u) + 7 - (sample & 7))) & 1;
    }
    // virtualPackedMonoSetBitForeground: set bits take register 490, clear bits 491, as RGB565.
    // 490 and 491 as the copy and A4 producers have met them: ffffffff and 0 from the setup, or 0 and
    // 0000ffff left by the ROM's 1bpp text draw. The ROM wrote nothing else to them from boot to the launcher.
    bool staleTextColours() const {
        return known[0x490] && known[0x491] && ((values[0x490] == 0xffffffff && !values[0x491]) ||
                                                (!values[0x490] && values[0x491] == 0xffff));
    }
    uint16_t virtualPackedMonoColour(bool bit) const { return uint16_t(values[bit ? 0x490 : 0x491]); }
    // virtualPackedMonoOpaqueCopy: every visible pixel replaces the destination.
    // virtualPackedMonoZeroPhase: bit 0 of each row lands on the encoded X, with no other offset.
    // Explicit virtual normalisation and truncation in native 5/6/5 channels.
    // alpha is used as it is, as the host GPU's blend does, so a fill with a wider value gives the same on both.
    static uint32_t virtualA4Blend(uint32_t destination, uint32_t source, uint32_t alpha) {
        return blendChannels(destination, source, alpha, alpha, alpha);
    }
    // The same for an alpha per channel, as 484 holds it: 4 bits each, red in 16-19, green 8-11, blue 0-3
    // (2009c1a0 scales each 8 bit channel of TwGfxBlendBlt's alpha by 15 / 255, rounding down).
    static uint32_t virtualBlend(uint32_t destination, uint32_t source, uint32_t alphas) {
        return blendChannels(destination, source, alphas & 15, (alphas >> 8) & 15, (alphas >> 16) & 15);
    }
    static uint32_t blendChannels(uint32_t destination, uint32_t source, uint32_t blue, uint32_t green, uint32_t red) {
        uint32_t result = 0;
        for (unsigned shift : {0u, 5u, 11u}) {
            const uint32_t mask = shift == 5 ? 63 : 31;
            const uint32_t alpha = shift == 0 ? blue : shift == 5 ? green : red;
            const uint32_t d = (destination >> shift) & mask, s = (source >> shift) & mask;
            result |= ((d * (15 - alpha) + s * alpha) / 15) << shift;
        }
        return result;
    }
    // Source 2 of the constant blend. Its origin follows the destination's clipping, as A4's does. The ROM sends
    // 420/421 as the rectangle's size and source 1 (46b/46c) as source 2's surface; other values are refused.
    Imx1BootstrapAccess blendSource(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                    const Imx1GraphicsMemory& memory, uint32_t wh, Fill& fill) const {
        using A = Imx1BootstrapAccess;
        const uint32_t pitch = values[0x419], width = wh >> 16, height = wh & 0xffff;
        const uint32_t sx = values[0x416] >> 16, sy = values[0x416] & 0xffff;
        if (values[0x420] != width || values[0x421] != height || values[0x46b] != values[0x418] || values[0x46c] != pitch)
            return refuse("blend-words", values[0x416]);
        if (!pitch || pitch > 8192 || sx + width > pitch || sy + height > 8191) return refuse("blend-geometry", values[0x416]);
        const uint64_t source = uint64_t(values[0x418]) + (uint64_t(sy) * pitch + sx) * 2;
        const uint64_t sourceLength = (uint64_t(height - 1) * pitch + width) * 2;
        if (source > 0xffffffffu || (source & 1) || !memory.contains(configuration, eim, uint32_t(source), sourceLength))
            return A::sequence;
        fill.a4 = true;
        fill.colour = values[0x484];
        fill.sourcePitch = pitch * 2;
        if (!fill.width || !fill.height) return A::ok;
        fill.sourceBase = uint32_t(source + uint64_t(fill.sourceY) * fill.sourcePitch + fill.sourceX * 2);
        // Reading what the blend writes needs an order the driver never relies on. Refused.
        const Area from{source, uint64_t(pitch) * 2, uint64_t(width) * 2, height};
        const Area to{fill.base, fill.pitch, uint64_t(fill.width) * 2, fill.height};
        return from.overlaps(to) ? refuse("blend-overlap", values[0x416]) : A::ok;
    }
    Imx1BootstrapAccess a4Source(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                               const Imx1GraphicsMemory& memory, uint32_t xy,
                               uint32_t width, uint32_t height, Fill& fill) const {
        using A = Imx1BootstrapAccess;
        // 1:1 rectangle extents. 416 is source 2's origin, x << 16 | y, as the constant blend sends it (the same
        // routine, 200a9998-200a99c4 here).
        // ASSUMPTION "A4 source origin per packet": the routine splits a mask taller than the ring holds into bands
        // and sends the same 416 with each; each packet reads source 2 from 416. Only a mask of more than about 270
        // rows at full width splits.
        const uint32_t pitch = values[0x419], destinationPitch = values[0x402];
        const uint32_t sx = values[0x416] >> 16, sy = values[0x416] & 0xffff;
        if (values[0x420] != width || values[0x421] != height ||
            !pitch || pitch > 8192 || sx + width > pitch || width > 8191 || height > 8191 || sy + height > 8191 ||
            values[0x46b] != values[0x418] || values[0x46c] != pitch) return A::sideEffect;
        const uint64_t source = uint64_t(values[0x418]) + (uint64_t(sy) * pitch + sx) * 2;
        const uint64_t sourceLength = (uint64_t(height - 1) * pitch + width) * 2;
        const uint64_t destination = uint64_t(values[0x401]) +
            (uint64_t(xy >> 16) * destinationPitch + (xy & 0xffff)) * 2;
        const uint64_t destinationLength = (uint64_t(height - 1) * destinationPitch + width) * 2;
        if (source > 0xffffffffu || (source & 1) || destination > 0xffffffffu || (destination & 1) ||
            !memory.contains(configuration, eim, uint32_t(source), sourceLength) ||
            !memory.contains(configuration, eim, uint32_t(destination), destinationLength)) return A::sequence;
        if (source < destination + destinationLength && destination < source + sourceLength) return A::sideEffect;
        fill.a4 = true;
        fill.sourcePitch = pitch * 2;
        if (fill.width && fill.height)
            fill.sourceBase = uint32_t(source + uint64_t(fill.sourceY) * fill.sourcePitch + fill.sourceX * 2);
        return A::ok;
    }
    Imx1BootstrapAccess rectangle(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                   const Imx1GraphicsMemory& memory, uint32_t xy, uint32_t wh, Fill& fill, bool host, bool copy = false, bool a4 = false, bool plain = false, bool mono = false, bool blend = false) const {
        using A = Imx1BootstrapAccess;
        for (auto reg : registers) if (reg != 0x48f && reg != 0x422 && (reg < 0x410 || reg > 0x412) && (reg < 0x464 || reg > 0x466) &&
            (a4 || blend || (reg != 0x418 && reg != 0x419 && reg != 0x420 && reg != 0x421 && reg != 0x416)) && !known[reg]) return A::sequence;
        const auto gmc = values[0x41b];
        // Host SRCCOPY is limited to the observed native RGB565 control words.
        // ASSUMPTION "host upload takes its source type from 41b": the ROM sends RGB565 rows with 4b1 source
        // type 0 left by the stretch restore (200b0b48 on), so 4b1 may hold type 3 or 0 here.
        if (host && !a4 && !mono && (gmc != 0x5798b4df || values[0x4b2] != 0x00cc0b00 ||
                     (values[0x4b1] != 0x40030d04 && values[0x4b1] != 0x40000d04) || values[0x472] != 0x3f))
            return refuse("host-upload-state", values[0x4b1]);
        // Admitted GMC and shadow words are those of the 9 November 2026 empty-card run.
        // The producer changes its datatype shadow without emitting it, so that field is not compared.
        if (mono && (gmc != 0x579804df || values[0x4b2] != 0x00cc0b00 ||
                     (values[0x4b1] & ~0x00070000u) != 0x40000d04 || values[0x472] != 0x3f)) return A::sideEffect;
        // A keyed copy keeps the ROP that was set: cc, or f0 with the solid brush (200aa2e4-200aa2f4).
        const bool keyedPaint = copy && !plain && gmc == 0x45e0b4df && values[0x4b2] == 0x00f00a00;
        if (copy && !plain && gmc != 0x4598b4df && !keyedPaint) return refuse("keyed-copy-41b", gmc);
        // The bitmap routine's invert and paint inverse copies (ROP 99 and 33, 2008a7b8) are not drawn.
        if (copy && plain && gmc != 0x5598b4df) return refuse("plain-copy-41b", gmc);
        if (copy && !keyedPaint && values[0x4b2] != 0x00cc0a00) return A::sideEffect;
        if (copy && (
                     values[0x4b1] != 0x40030d04 || values[0x472] != 0x3f ||
                     values[0x471] != 0x1fff1fff)) return A::sideEffect;
        if (plain && (values[0x4b3] != 0xffffffff || values[0x41d] ||
                      values[0x41e] || values[0x30f] != 0x20)) return A::sideEffect;
        // Only native solid PATCOPY in RGB565, without transforms or blending.
        // Solid brush with ROP3 f0 (pattern) or 5a (destination xor pattern), the two the fill routine's
        // caller sets with a solid colour (2008a97c, 2008a0c8).
        const uint32_t rop = (gmc >> 17) & 255;
        // The constant blend's state as 200a94cc-200a95b0 sets it after ROP cc (2009c5cc): the A4 compositor's GMC, MIX
        // and datatypes, with E2 488 bit 19 (alpha from the mask) clear. 484 is any global alpha.
        if (blend && (gmc != 0xd798d4df || values[0x4b2] != 0x01cc1300 || values[0x4b1] != 0x40057d04 ||
            values[0x472] != 0x3f || values[0x471] != 0x1fff1fff || values[0x488] != 4 || !known[0x48f] ||
            values[0x48f] != 0xffffffff || values[0x4b3] != 0xffffffff || values[0x41d] || values[0x30f] != 0x20 ||
            (values[0x484] & ~0x000f0f0fu))) return refuse("blend-state", gmc);
        if (!host && !copy && !blend && (((gmc >> 4) & 15) != 13 || ((gmc >> 8) & 15) != 4 ||
            (rop != 0xf0 && rop != 0x5a) || !(gmc & (1u << 28)) ||
            !(gmc & (1u << 30)) || !(gmc & (1u << 15)) || (gmc & 0x80000000u) || values[0x488] != 4 ||
            (gmc & (1u << 16)) || !(gmc & 2) || values[0x4a1] || values[0x4a0])) return A::sideEffect;
        if (values[0x488] != (a4 ? 0x00080004u : 4u) || values[0x4a1] || values[0x4a0]) return A::sideEffect;
        // The A4 producer never sends 490 or 491. The policy composes source2 through the mask and reads neither.
        // With 488 bit 19 the mask is the alpha and 484 isn't read: MaskBlendBlt (2009af14) never sets 484, so it
        // holds whatever the last BlendBlt (2009c1a0) left there.
        if (a4 && (!staleTextColours() || gmc != 0xd798d4df || values[0x4b2] != 0x01cc1300 ||
            values[0x4b1] != 0x40057d04 || values[0x472] != 0x3f || values[0x471] != 0x1fff1fff ||
            !known[0x48f] || values[0x48f] != 0xffffffff ||
            values[0x4b3] != 0xffffffff || values[0x41d] || values[0x30f] != 0x20)) return A::sideEffect;
        const auto datatype = values[0x4b1], mix = values[0x4b2], direction = values[0x472];
        if (!host && !copy && !blend && ((datatype & 15) != 4 || ((datatype >> 8) & 15) != 13 || !(datatype & (1u << 30)) ||
            (datatype & ~0x4007ff0fu) || ((mix >> 16) & 255) != rop || (mix & ~0x00ff3f00u) ||
            (direction & 3) != 3 || (direction & ~0x3fu))) return A::sideEffect;
        for (auto reg : {0x42e,0x42f,0x430,0x431,0x432,0x433}) if (values[reg]) return A::sideEffect;
        if ((values[0x46f] | values[0x470]) & ~0x1fff1fffu) return A::sideEffect;
        const uint32_t x = host ? xy & 0xffff : xy >> 16, y = host ? xy >> 16 : xy & 0xffff;
        const uint32_t width = host ? wh & 0xffff : wh >> 16, height = host ? wh >> 16 : wh & 0xffff;
        const uint32_t pitch = values[0x402];
        // With the scissor on (GMC bit 3) the bounds apply to what is left after it: the software stretch's
        // runs (200b0644 from 200b0c10) may reach past a small surface, as TwGfxStretchBlt2 allows.
        if (!width || !height || !pitch || pitch > 8192 || x + width > 8192 || y + height > 8192 ||
            (!(gmc & 8) && (x + width > pitch ||
                            !configuration.engineState().clipContains(x + width - 1, y + height - 1)))) return A::sideEffect;
        uint32_t left = x, top = y, right = x + width, bottom = y + height;
        if (gmc & 8) {
            left = std::max(left, values[0x46f] & 0x1fff);
            top = std::max(top, (values[0x46f] >> 16) & 0x1fff);
            // ASSUMPTION "scissor far edge not included": 470 holds the first column and row not drawn.
            // The driver writes its clip rectangle's right and bottom there as they are (200af614-200af63c),
            // the surface's width and height for a whole surface (200af5b4-200af5dc), and sends a 16 bit
            // upload of odd width one pixel wider for the scissor to trim (200abde4).
            right = std::min(right, values[0x470] & 0x1fff);
            bottom = std::min(bottom, (values[0x470] >> 16) & 0x1fff);
        }
        if ((gmc & 8) && left < right && top < bottom &&
            (right > pitch || !configuration.engineState().clipContains(right - 1, bottom - 1))) return A::sideEffect;
        fill.sourceX = left - x;
        fill.sourceY = top - y;
        fill.colour = values[0x41f] & 0xffff;
        fill.pitch = pitch * 2;
        if (left >= right || top >= bottom) return A::ok;
        const uint64_t address = uint64_t(values[0x401]) + uint64_t(top) * fill.pitch + left * 2;
        if (address > 0xffffffffu || (address & 1)) return A::sideEffect;
        fill.base = uint32_t(address);
        fill.width = right - left;
        fill.height = bottom - top;
        return memory.contains(configuration,eim,fill.base,
            uint64_t(fill.height - 1) * fill.pitch + fill.width * 2) ? A::ok : A::sequence;
    }
    // One segment of the line packet, as runs of pixels in fills (rows for an x-major line, columns for a
    // y-major one), drawn like a solid fill: same state, scissor and bounds.
    // ASSUMPTION "line ends drawn": both ends are drawn. The driver repeats the end point after each segment
    // (200aff74-200aff98), which only makes sense if the chip leaves out a segment's last pixel and draws a
    // zero length one as one pixel.
    // A pixel is the one nearest the line across the major axis. Where the line passes exactly half way
    // between two (a Bresenham tie) a line going down (y increasing) keeps the minor coordinate it had and one
    // going up takes the next one.
    // ASSUMPTION "line ties as the Radeon two-point line": the chip's 2D engine is the ATI one the Radeon has (the
    // W100 register map), and the X Radeon driver sets the zero-line bias to octants 5 to 8 (y increasing) so
    // software lines match its hardware lines (radeon_accelfuncs.c, miSetZeroLineBias). Neither TwGfx nor the
    // ROM says; the ROM's clipper (200afba8) rounds its intersections toward zero with no tie care.
    // Only ROP f0 is drawn: with 5a the end pixel's two draws would matter.
    Imx1BootstrapAccess line(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                             const Imx1GraphicsMemory& memory, uint32_t start, uint32_t end, std::vector<Fill>& fills) {
        using A = Imx1BootstrapAccess;
        if (((values[0x41b] >> 17) & 255) != 0xf0) return refuse("line-rop", (values[0x41b] >> 17) & 255);
        const int32_t x0 = start & 0xffff, y0 = start >> 16, x1 = end & 0xffff, y1 = end >> 16;
        if (x0 > 8191 || y0 > 8191 || x1 > 8191 || y1 > 8191) return refuse("line-points", end);
        const int32_t dx = x1 - x0, dy = y1 - y0;
        const bool xMajor = std::abs(dx) >= std::abs(dy);
        const int64_t major = xMajor ? std::abs(dx) : std::abs(dy), minor = xMajor ? std::abs(dy) : std::abs(dx);
        const int32_t majorStep = (xMajor ? dx : dy) < 0 ? -1 : 1, minorStep = (xMajor ? dy : dx) < 0 ? -1 : 1;
        // The bias takes a tie down to the lower offset: (2 i minor + major) is a multiple of 2 major only there.
        const int64_t bias = dy > 0 ? 1 : 0;
        // Runs of pixels that share the minor coordinate, in drawing order.
        int64_t runStart = 0;
        for (int64_t i = 0; i <= major; ++i) {
            const int64_t offset = major ? (2 * i * minor + major - bias) / (2 * major) : 0;
            const int64_t next = i < major ? (2 * (i + 1) * minor + major - bias) / (2 * major) : -1;
            if (next == offset) continue;
            // Pixels runStart to i of the major axis at this minor offset.
            const int64_t first = runStart, count = i - runStart + 1;
            runStart = i + 1;
            int64_t along = (xMajor ? x0 : y0) + majorStep * first;
            if (majorStep < 0) along -= count - 1;
            const int64_t across = (xMajor ? y0 : x0) + minorStep * offset;
            const uint32_t x = uint32_t(xMajor ? along : across), y = uint32_t(xMajor ? across : along);
            const uint32_t width = xMajor ? uint32_t(count) : 1, height = xMajor ? 1 : uint32_t(count);
            Fill fill{};
            const auto status = rectangle(configuration, eim, memory, x << 16 | y, width << 16 | height, fill, false);
            if (status != A::ok) return status;
            ticks += uint64_t(fill.width) * fill.height;
            fills.push_back(std::move(fill));
        }
        return A::ok;
    }
    Imx1BootstrapAccess refuse(const char* what, uint32_t value) const {
        refused = {what, value};
        return Imx1BootstrapAccess::sideEffect;
    }
    // TwGfxTransformBlt with a rotation or mirror (200aa564, from 2009b538): the copy state with DP_CNTL (472) giving
    // the directions, then 464 (source x << 16 | y on 46b/46c), 465 (the destination pixel the first source pixel
    // goes to) and 466 (destination width << 16 | height), whose write starts it. The source is read a row at a
    // time from its top left (472 bits 2, 3 and 5 set). Bit 4 set steps the destination along x for each source
    // pixel and along y for each source row, clear the other way round; bits 0 and 1 set step x and y up, clear
    // down. The ROM sends 2e, 3c, 2d, 3e, 3d, 2f and 2c (200aa6d8-200aa720); with 3f it is a plain copy.
    // The scissor drops destination pixels as it does for a copy.
    // ASSUMPTION "transform from DP_CNTL": the bit meanings are the W100's DP_CNTL (dst/src x and y direction,
    // dst/src major x). The ROM's corner and size for each case match them and TwGfx.h's table of results.
    Imx1BootstrapAccess transformBlit(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                      const Imx1GraphicsMemory& memory, Fill& fill) const {
        using A = Imx1BootstrapAccess;
        for (auto reg : {0x4a1,0x41b,0x472,0x4b2,0x4b1,0x46f,0x470,0x401,0x402,0x46b,0x46c,0x488,0x471,0x4a0,0x48f,
                         0x4b3,0x41d,0x41e,0x30f,0x464,0x465,0x466,0x42e,0x42f,0x430,0x431,0x432,0x433})
            if (!known[reg]) return A::sequence;
        // 200aa744-200aa7ac: GMC (x & ~8e007000) | 54003000, DP_MIX (x & ~c1000700) | 80000200 after ROP cc
        // (2009c5cc), source datatype 3. The rest as the plain copy has it. Only the GMC the copy state leaves
        // (5598b4df) is taken; 4b1's source 2 type (bits 12-14) is unused by a copy.
        const uint32_t gmc = values[0x41b], mix = values[0x4b2], control = values[0x472];
        if (gmc != 0x5598b4df) return refuse("transform-state", gmc);
        if ((mix & 0xc1ff0700u) != 0x80cc0200u || (values[0x4b1] & ~0x7000u) != 0x40030d04 ||
            values[0x488] != 4 || values[0x471] != 0x1fff1fff || values[0x48f] != 0xffffffff ||
            values[0x4b3] != 0xffffffff || values[0x41d] ||
            values[0x41e] || values[0x30f] != 0x20 || values[0x4a0] || values[0x4a1] ||
            (control != 0x2e && control != 0x3c && control != 0x2d && control != 0x3e && control != 0x3d &&
             control != 0x2f && control != 0x2c && control != 0x3f)) return refuse("transform-state", control);
        for (auto reg : {0x42e,0x42f,0x430,0x431,0x432,0x433}) if (values[reg]) return refuse("transform-state", reg);
        if ((values[0x46f] | values[0x470]) & ~0x1fff1fffu) return refuse("transform-state", 0x46f);
        const bool majorX = control & 0x10, right = control & 1, down = control & 2;
        const uint32_t sx = values[0x464] >> 16, sy = values[0x464] & 0xffff;
        const uint32_t x0 = values[0x465] >> 16, y0 = values[0x465] & 0xffff;
        const uint32_t width = values[0x466] >> 16, height = values[0x466] & 0xffff;
        const uint32_t sourceWidth = majorX ? width : height, sourceHeight = majorX ? height : width;
        const uint32_t pitch = values[0x402], sourcePitch = values[0x46c];
        // The destination rectangle the start corner and directions cover. TwGfxTransformBlt (2009b2f0) checks
        // only the source rectangle against its surface, and 200aa564 only that the destination meets the clip
        // rectangle (200a9c00), so the destination may reach past the surface and the scissor trims it, as for
        // the plain copy: the surface bounds apply to what is left.
        const uint32_t x = right ? x0 : x0 - (width - 1), y = down ? y0 : y0 - (height - 1);
        if (!width || !height || x0 > 8191 || y0 > 8191 || (!right && x0 < width - 1) || (!down && y0 < height - 1) ||
            !pitch || pitch > 8192 || x + width > 8192 || y + height > 8192 ||
            !sourcePitch || sourcePitch > 8192 || sx + sourceWidth > sourcePitch || sy + sourceHeight > 8191)
            return refuse("transform-geometry", values[0x466]);
        // GMC 5598b4df has the scissor on (bit 3).
        const uint32_t left = std::max(x, values[0x46f] & 0x1fff), top = std::max(y, (values[0x46f] >> 16) & 0x1fff);
        const uint32_t end = std::min(x + width, values[0x470] & 0x1fff);
        const uint32_t bottom = std::min(y + height, (values[0x470] >> 16) & 0x1fff);
        if (left < end && top < bottom && (end > pitch || !configuration.engineState().clipContains(end - 1, bottom - 1)))
            return refuse("transform-geometry", values[0x466]);
        const uint64_t source = uint64_t(values[0x46b]) + (uint64_t(sy) * sourcePitch + sx) * 2;
        const uint64_t sourceLength = (uint64_t(sourceHeight - 1) * sourcePitch + sourceWidth) * 2;
        if (source > 0xffffffffu || (source & 1) || !memory.contains(configuration, eim, uint32_t(source), sourceLength)) return A::sequence;
        fill.pitch = pitch * 2;
        fill.plain = fill.keyEqual = true;
        if (left >= end || top >= bottom) return A::ok;
        const uint64_t address = uint64_t(values[0x401]) + uint64_t(top) * fill.pitch + left * 2;
        if (address > 0xffffffffu || (address & 1)) return refuse("transform-geometry", values[0x465]);
        fill.base = uint32_t(address);
        fill.width = end - left;
        fill.height = bottom - top;
        if (!memory.contains(configuration, eim, fill.base, uint64_t(fill.height - 1) * fill.pitch + fill.width * 2)) return A::sequence;
        // Source column i and row j of the destination pixel at (left, top), and how they move for each
        // destination column and row.
        const int64_t dx = right ? 1 : -1, dy = down ? 1 : -1;
        const int64_t u = (int64_t(left) - x0) * dx, v = (int64_t(top) - y0) * dy;
        const int64_t i = majorX ? u : v, j = majorX ? v : u;
        const int64_t row = int64_t(sourcePitch) * 2;
        fill.sourceBase = uint32_t(int64_t(source) + j * row + i * 2);
        fill.colour = uint32_t(int32_t(majorX ? dx * 2 : dx * row));
        fill.sourcePitch = uint32_t(int32_t(majorX ? dy * row : dy * 2));
        // Reading what the blit writes needs an order the driver never relies on. Refused.
        const Area from{source, uint64_t(sourcePitch) * 2, uint64_t(sourceWidth) * 2, sourceHeight};
        const Area to{fill.base, fill.pitch, uint64_t(fill.width) * 2, fill.height};
        return from.overlaps(to) ? refuse("transform-overlap", values[0x465]) : A::ok;
    }
    // The chip's stretch blit as FUN_200b0644 sends it (220 at 200b0b14): source surface 46b/46c from
    // 464 in quarter pixels, destination 401/402 at 465, output size in 466, 8.8 ratios in 412 (x low, y high,
    // source pixels per destination pixel, the ROM keeps them in 10 to 1ff hex), with 416 to 422 repeating them
    // (200b0998-200b0acc). It writes destination memory like a copy.
    // Destination column i of the unclipped rectangle reads source column i * ratio >> 8 from the origin, and the
    // same for rows: at ratio 80 hex each source pixel twice, which the bitmap routine's half density bitmaps
    // (2008a72c) and its keyed modes must have.
    // ASSUMPTION "stretch steps from the origin": the step starts at 464 with no half pixel offset; 464 carries
    // the start phase in quarter pixels and the ROM only sends whole pixels (464 & 30003 is always 0).
    // Smooth (488 bit 0 clear, twGfxStretchSmooth) filters through source 2, one row down (416); its filter is
    // not known and stays refused.
    Imx1BootstrapAccess stretchBlit(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                    const Imx1GraphicsMemory& memory, Fill& fill) const {
        using A = Imx1BootstrapAccess;
        for (auto reg : {0x4a1,0x41b,0x472,0x4b2,0x4b1,0x46f,0x470,0x401,0x402,0x46b,0x46c,0x488,0x471,0x4a0,0x48f,
                         0x416,0x418,0x419,0x410,0x411,0x412,0x420,0x421,0x422,0x464,0x465,0x466,
                         0x42e,0x42f,0x430,0x431,0x432,0x433}) if (!known[reg]) return A::sequence;
        // The state words 200b0830-200b08dc leave: ROP cc, colour sources, scissor on, left to right and down.
        // The ROM sets GMC to (GMC & ~8e007000) | d4003000 in its shadow but sends it only when another word in
        // the same group changed, so the chip may hold the GMC of the operation before. Its source and ROP
        // fields alias DP_MIX, which the stretch always sends after it, so only the rest must match: the
        // clipping, datatypes and the bits above the source field. And DP_MIX must be current (mixAlias).
        const uint32_t gmc = values[0x41b];
        if ((gmc & 0x7001ffffu) != 0x5000b4dfu || !mixAlias(gmc)) return refuse("stretch-state", 0x41b);
        if (values[0x488] == 0x02020008) return refuse("stretch-smooth", values[0x412]);
        for (auto item : {std::pair<uint32_t,uint32_t>{0x4b2,0x01cc1200}, {0x4b1,0x40037d04},
                 {0x472,0x3f}, {0x488,0x02020009}, {0x471,0x1fff1fff}, {0x48f,0xffffffff}, {0x4a1,0}, {0x4a0,0},
                 {0x42e,0}, {0x42f,0}, {0x430,0}, {0x431,0}, {0x432,0}, {0x433,0}})
            if (values[item.first] != item.second) return refuse("stretch-state", item.first);
        if ((values[0x416] >> 16) != (values[0x464] >> 16) || (values[0x416] & 0xffff) != ((values[0x464] + 4) & 0xffff) ||
            values[0x418] != values[0x46b] || values[0x419] != values[0x46c] || values[0x420] != values[0x410] ||
            values[0x421] != values[0x411] || values[0x422] != values[0x412] ||
            values[0x410] != (values[0x466] >> 16) + 1 || values[0x411] != (values[0x466] & 0xffff) ||
            (values[0x464] & 0x00030003) || ((values[0x46f] | values[0x470]) & ~0x1fff1fffu))
            return refuse("stretch-words", values[0x464]);
        const uint32_t ratioX = values[0x412] & 0xffff, ratioY = values[0x412] >> 16;
        if (ratioX < 0x10 || ratioX > 0x1ff || ratioY < 0x10 || ratioY > 0x1ff) return refuse("stretch-ratio", values[0x412]);
        const uint32_t sx = values[0x464] >> 18, sy = (values[0x464] & 0xffff) >> 2;
        const uint32_t x = values[0x465] >> 16, y = values[0x465] & 0xffff;
        const uint32_t width = values[0x466] >> 16, height = values[0x466] & 0xffff;
        const uint32_t pitch = values[0x402], sourcePitch = values[0x46c];
        const uint32_t sourceWidth = width ? stretchStep(width - 1, ratioX) + 1 : 0;
        const uint32_t sourceHeight = height ? stretchStep(height - 1, ratioY) + 1 : 0;
        if (!width || !height || !pitch || pitch > 8192 || x + width > pitch || y + height > 8192 ||
            !configuration.engineState().clipContains(x + width - 1, y + height - 1) ||
            !sourcePitch || sourcePitch > 8192 || sx + sourceWidth > sourcePitch || sy + sourceHeight > 8191)
            return refuse("stretch-geometry", values[0x466]);
        const uint32_t left = std::max(x, values[0x46f] & 0x1fff), top = std::max(y, (values[0x46f] >> 16) & 0x1fff);
        const uint32_t right = std::min(x + width, values[0x470] & 0x1fff), bottom = std::min(y + height, (values[0x470] >> 16) & 0x1fff);
        const uint64_t source = uint64_t(values[0x46b]) + (uint64_t(sy) * sourcePitch + sx) * 2;
        const uint64_t sourceLength = (uint64_t(sourceHeight - 1) * sourcePitch + sourceWidth) * 2;
        if (source > 0xffffffffu || (source & 1) || !memory.contains(configuration, eim, uint32_t(source), sourceLength)) return A::sequence;
        fill.sourceX = left - x;
        fill.sourceY = top - y;
        fill.pitch = pitch * 2;
        fill.sourcePitch = sourcePitch * 2;
        fill.sourceBase = uint32_t(source);
        fill.plain = fill.doubled = true;
        fill.paint = values[0x412] == 0x00800080 ? 0 : values[0x412];
        if (left >= right || top >= bottom) return A::ok;
        const uint64_t address = uint64_t(values[0x401]) + uint64_t(top) * fill.pitch + left * 2;
        if (address > 0xffffffffu || (address & 1)) return refuse("stretch-geometry", values[0x465]);
        fill.base = uint32_t(address);
        fill.width = right - left;
        fill.height = bottom - top;
        const uint64_t length = uint64_t(fill.height - 1) * fill.pitch + fill.width * 2;
        if (!memory.contains(configuration, eim, fill.base, length)) return A::sequence;
        // Reading a source the blit is writing needs an order the driver never relies on. Refused.
        const Area from{source, uint64_t(sourcePitch) * 2, uint64_t(sourceWidth) * 2, sourceHeight};
        const Area to{fill.base, fill.pitch, uint64_t(fill.width) * 2, fill.height};
        return from.overlaps(to) ? refuse("stretch-overlap", values[0x465]) : A::ok;
    }
    std::array<uint32_t,0x800> values{};
    std::array<bool,0x800> known{};
    std::deque<Batch> batches;
    uint32_t queuedWords = 0;
    uint64_t ticks = 0;
    mutable Refusal refused;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsCommands) == 10352, "update state() and stateVersion");
#endif
#endif
