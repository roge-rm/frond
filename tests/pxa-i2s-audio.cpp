// Firmware-free controller, codec and final queue tests. Not guest qualification.
#include <cassert>
#include <cstdlib>
#include <vector>
#include "pxa_I2S.h"
#include "i2sdev_AK4534.h"
#include "audio_queue.h"
#include "savestate/savestateAll.h"
#include "Tg50Chunks.h"
#include "Tg50Audio.h"

static ArmMemAccessF mmio;
static void *controller;
static I2cDeviceActionF i2c;
static void *codec;
static bool irq, txRequest, rxRequest;
static std::vector<uint32_t> serial;
static uint32_t lastNumerator, lastDenominator;
bool memRegionAdd(ArmMem *, uint32_t, uint32_t, ArmMemAccessF f, void *context) {
    mmio = f; controller = context; return true;
}
bool pxaI2cDeviceAdd(PxaI2c *, I2cDeviceActionF f, void *context) {
    i2c = f; codec = context; return true;
}
void pxaIcInt(PxaIc *, uint_fast8_t, bool value) { irq = value; }
void pxaDmaExternalReq(PxaDma *, uint_fast8_t request, bool value) {
    if (request == DMA_CMR_I2S_TX) txRequest = value;
    if (request == DMA_CMR_I2S_RX) rxRequest = value;
}
extern "C" void uarmAbort() { std::abort(); }
static void wr(unsigned offset, uint32_t value) {
    assert(mmio(controller, 0x40400000 + offset, 4, true, &value));
}
static uint32_t rd(unsigned offset) {
    uint32_t value{}; assert(mmio(controller, 0x40400000 + offset, 4, false, &value)); return value;
}
static uint32_t frame(void *, uint32_t tx, bool, bool, uint32_t n, uint32_t d) {
    serial.push_back(tx); lastNumerator = n; lastDenominator = d; return 0x89abcdef;
}
static void reg(unsigned address, unsigned value) {
    i2c(codec, i2cStart, 0); assert(i2c(codec, i2cTx, 0x20));
    assert(i2c(codec, i2cTx, address)); assert(i2c(codec, i2cTx, value));
    i2c(codec, i2cStop, 0);
}
static void controllerChecks(PxaI2s *link) {
    pxaI2sAttach(link, nullptr, frame, nullptr);
    pxaI2sSetExternalRate(link, 44100);
    wr(0, 0x7700); wr(0x14, 0x78);
    assert(!irq && !txRequest && !rxRequest);
    pxaI2sAdvance(link, 1000000000); assert(serial.empty());
    for (unsigned i = 0; i < 8; ++i) wr(0x80, 0x80000001 + i);
    wr(0, 0x7701); assert(!txRequest);
    pxaI2sPeriodic(link); assert(txRequest && serial.back() == 0x80000001);
    assert((rd(0xc) & 2) && rd(0x80) == 0x89abcdef); // no implicit TX loopback
    wr(4, 0x20); pxaI2sPeriodic(link); assert(rd(0x80) == 0x80000002);
    wr(4, 0x18); assert(!txRequest && !rxRequest && !(rd(0xc) & 0xff02));
    assert(rd(0x80) == 0);
    wr(4, 8); wr(0x80, 0xabcd1234); pxaI2sPeriodic(link); pxaI2sPeriodic(link);
    assert(serial.back() == 0xabcd1234 && (rd(0xc) & 0x20) && irq);
    wr(0x18, 0x20); assert(!(rd(0xc) & 0x20));
    wr(0, 8); assert(rd(4) == 0 && rd(0x60) == 26 && rd(0xc) == 1 && !irq);
    const unsigned rates[] = {44100,22050,11025,48000,32000,24000,16000,8000};
    for (auto rate : rates) {
        wr(0, 0x7700); pxaI2sSetExternalRate(link, rate); wr(0, 0x7701);
        serial.clear();
        for (unsigned t = 0; t < 50000; ++t) pxaI2sAdvance(link, 20000);
        assert(serial.size() == rate && lastNumerator == rate && lastDenominator == 1);
    }
    wr(0, 0x7705); wr(0x60, 13); serial.clear();
    for (unsigned t = 0; t < 50000; ++t) pxaI2sAdvance(link, 20000);
    assert(serial.size() == 576000 / 13 && lastDenominator == 13);
    pxaI2sSetClockEnabled(link, false); const auto size = serial.size();
    pxaI2sAdvance(link, 1000000000); assert(serial.size() == size && !txRequest);
    pxaI2sSetClockEnabled(link, true);
    wr(0, 8); wr(0, 0x7700);
}
struct AudioState {
    PxaI2s *link;
    AK4534 *ak;
    template <typename T> void Save(T &save) { pxaI2sSave(link, save); ak4534Save(ak, save); }
    void Load(SavestateLoader<ChunkType> &loader) {
        pxaI2sLoad(link, loader);
        ak4534Load(ak, loader);
        pxaI2sRefresh(link);
    }
};
static std::vector<uint32_t> saveState(AudioState &state) {
    Savestate<ChunkType> save; assert(save.Save(state));
    const auto *data = static_cast<uint32_t *>(save.GetBuffer());
    return {data, data + save.GetSize() / 4};
}
static uint32_t *chunkData(std::vector<uint32_t> &data, ChunkType wanted) {
    auto *next = data.data() + 1 + 2 * data[0];
    for (unsigned i = 0; i < data[0]; ++i) {
        if (data[1 + i*2] == static_cast<uint32_t>(wanted)) return next;
        next += data[2 + i*2];
    }
    std::abort();
}
static void stateChecks(PxaI2s *link, AK4534 *ak, AudioQueue *queue) {
    reg(0, 0x80); reg(1, 0x61);
    reg(5, 7 << 5); reg(6, 0x10); reg(12, 12);
    wr(4, 8); wr(0, 0x7701); wr(0x80, 0x81234567);
    pxaI2sAdvance(link, 200000); // partial controller and resampler frames, active gain ramp
    AudioState state{link, ak}; auto saved = saveState(state);
    audioQueueClear(queue);
    pxaI2sAdvance(link, 1000000);
    uint32_t expected[32]{}; const auto count = audioQueuePopChunk(queue, expected, 32);
    assert(count > 0);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(saved.data(), saved.size()*4, state));
    assert(audioQueuePendingSamples(queue) == 0);
    pxaI2sAdvance(link, 1000000);
    uint32_t actual[32]{}; assert(audioQueuePopChunk(queue, actual, 32) == count);
    for (unsigned i = 0; i < count; ++i) assert(expected[i] == actual[i]);
    auto invalid = saved;
    chunkData(invalid, ChunkType::pxaI2s)[2] |= 255; // impossible TX fill level
    assert(!loader.Load(invalid.data(), invalid.size()*4, state));
    assert(loader.Load(saved.data(), saved.size()*4, state));
    invalid = saved;
    chunkData(invalid, tg50CodecChunk)[23] = 65537; // gain exceeds unity
    assert(!loader.Load(invalid.data(), invalid.size()*4, state));
    assert(loader.Load(saved.data(), saved.size()*4, state));
    assert(!loader.Load(saved.data(), saved.size()*4-4, state));
    assert(loader.Load(saved.data(), saved.size()*4, state));
    // A fresh host queue attaches after startup load, preserving the partial frame.
    auto *freshLink = pxaI2sInit(nullptr, nullptr, nullptr);
    auto *freshCodec = ak4534Init(nullptr, freshLink, nullptr);
    AudioState fresh{freshLink, freshCodec};
    assert(loader.Load(saved.data(), saved.size()*4, fresh));
    auto *freshQueue = audioQueueCreate(32);
    ak4534SetAudioQueue(freshCodec, freshQueue);
    pxaI2sAdvance(freshLink, 1000000);
    assert(audioQueuePopChunk(freshQueue, actual, 32) == count);
    for (unsigned i = 0; i < count; ++i) assert(expected[i] == actual[i]);
    ak4534Destroy(freshCodec);
}
static void codecChecks(PxaI2s *link) {
    auto *ak = ak4534Init(nullptr, link, nullptr);
    auto *queue = audioQueueCreate(32);
    ak4534SetAudioQueue(ak, queue);
    reg(0, 0x80); reg(1, 0x61); reg(4, 0x6a); reg(6, 0x10);
    wr(4, 8); wr(0, 0x7701);
    const unsigned rates[] = {44100,22050,11025,48000,32000,24000,16000,8000};
    for (unsigned fs = 0; fs < 8; ++fs) {
        reg(5, fs << 5); wr(0x80, 0xf83003e8); // left 1000, right -2000
        for (unsigned t = 0; t < 50000; ++t) pxaI2sAdvance(link, 20000);
        uint32_t frames[32]{};
        assert(audioQueuePendingSamples(queue) == 32); // bounded ring, final samples only
        const auto count = audioQueuePopChunk(queue, frames, 32); assert(count == 32);
        for (auto v : frames) assert(v == 0xf83003e8);
    }
    const auto before = audioQueueGetState(queue).generation;
    reg(5, 0); assert(audioQueueGetState(queue).generation > before);
    reg(12, 255); // linked full attenuation
    for (unsigned t = 0; t < 2500; ++t) pxaI2sAdvance(link, 20000);
    uint32_t frames[32]{}; assert(audioQueuePopChunk(queue, frames, 32) == 32);
    for (auto v : frames) assert(v == 0);
    reg(12, 0); reg(6, 0); reg(13, 255); // independent right attenuation
    for (unsigned t = 0; t < 2500; ++t) pxaI2sAdvance(link, 20000);
    assert(audioQueuePopChunk(queue, frames, 32) == 32);
    for (auto v : frames) assert(v == 1000);
    reg(6, 0x20); for (unsigned t = 0; t < 2500; ++t) pxaI2sAdvance(link, 20000);
    assert(audioQueuePopChunk(queue, frames, 32) == 32);
    for (auto v : frames) assert(v == 0);
    wr(4, 0); ak4534TestInput(ak, true, 0x12345678); pxaI2sPeriodic(link);
    assert(rd(0x80) == 0x12345678);
    ak4534TestInput(ak, false, 0); pxaI2sPeriodic(link); assert(rd(0x80) == 0);
    reg(1, 0); const auto generation = audioQueueGetState(queue).generation;
    pxaI2sAdvance(link, 1000000000); assert(audioQueuePendingSamples(queue) == 0);
    assert(audioQueueGetState(queue).generation == generation);
    stateChecks(link, ak, queue);
    ak4534Reset(ak); ak4534Destroy(ak);
}

struct MixedOutput {
    Tg50Audio mixer;
    AudioQueue *queue{};
    unsigned maxPending{}, dacFrames{};
    static void Frame(void *context, uint32_t packed) {
        auto &out = *static_cast<MixedOutput *>(context);
        assert(packed == 0xf83003e8); // Actual resampled codec left1000/right-2000.
        assert(out.mixer.PushDac(packed));
        ++out.dacFrames;
        out.maxPending = std::max(out.maxPending, out.mixer.GetState().dacCount);
    }
    static void Boundary(void *context) {
        auto &out = *static_cast<MixedOutput *>(context);
        out.mixer.CodecBoundary();
        if (out.queue) audioQueueClear(out.queue);
    }
    std::vector<uint32_t> Run(PxaI2s *link, unsigned ticks) {
        std::vector<uint32_t> frames;
        for (unsigned t = 0; t < ticks; ++t) {
            pxaI2sAdvance(link, 20000);
            mixer.Advance(20000, [&](uint32_t packed) {
                frames.push_back(packed);
                if (queue) audioQueuePush(queue, packed);
            });
        }
        return frames;
    }
};
static void mixedChecks() {
    const unsigned rates[] = {44100,22050,11025,48000,32000,24000,16000,8000};
    for (unsigned fs = 0; fs < 8; ++fs) {
        auto *link = pxaI2sInit(nullptr, nullptr, nullptr);
        auto *ak = ak4534Init(nullptr, link, nullptr);
        MixedOutput out;
        out.mixer.WritePwm({127,144,288});
        out.mixer.SetMuxOutput(true);
        out.mixer.Clock(true);
        out.queue = audioQueueCreate(8192);
        ak4534SetOutput(ak, &out, MixedOutput::Frame, MixedOutput::Boundary);
        // No codec AudioQueue: all final output must pass through the board mixer callback.
        reg(0, 0x80); reg(1, 0x61); reg(4, 0x6a); reg(6, 0x10); reg(5, fs << 5);
        wr(4, 8); wr(0, 0x7701); wr(0x80, 0xf83003e8);
        const auto frames = out.Run(link, 50000);
        assert(frames.size() == 44100);
        assert(out.dacFrames >= 44094 && out.dacFrames <= 44101);
        assert(out.maxPending > 0 && out.maxPending <= 7);
        if (rates[fs] == 8000) assert(out.maxPending >= 5);
        unsigned mixed{}, pwmRises{};
        bool previousPositive{};
        for (auto packed : frames) {
            const int left = int16_t(packed), right = int16_t(packed >> 16);
            const int difference = left - right;
            assert(difference == 3000 || difference == 0); // Bounded startup latency only.
            if (difference == 3000) {
                ++mixed;
                const int pwm = left - 1000;
                assert(pwm == right + 2000 && pwm >= -16384 && pwm <= 16384);
                if (pwm > 0 && !previousPositive) ++pwmRises;
                previousPositive = pwm > 0;
            }
        }
        assert(mixed >= 44094 && pwmRises >= 198 && pwmRises <= 200);
        // Select a checkpoint with queued DAC and nonzero converter/PWM/common phases.
        AudioState state{link, ak};
        std::vector<uint32_t> saved;
        bool partial{};
        for (unsigned t = 0; t < 1000; ++t) {
            pxaI2sAdvance(link, 20000);
            saved = saveState(state);
            const auto &m = out.mixer.GetState();
            const auto *codecState = chunkData(saved, tg50CodecChunk);
            // v1 stores the codec resampler phase after gains/targets/ramps and wire rate.
            const bool codecPartial = codecState[29] != 0;
            if (m.dacCount && m.pwmPhase && m.hostPhase &&
                (44100 % rates[fs] == 0 || codecPartial)) { partial = true; break; }
            out.mixer.Advance(20000, [](uint32_t) {});
        }
        assert(partial);
        const auto savedMixer = out.mixer.GetState();
        // The checkpoint is between source production and the common tick, with a real DAC FIFO.
        auto continuation = [](MixedOutput &sink, PxaI2s *wire) {
            std::vector<uint32_t> frames;
            sink.mixer.Advance(20000, [&](uint32_t packed) {
                frames.push_back(packed);
                if (sink.queue) audioQueuePush(sink.queue, packed);
            });
            auto rest = sink.Run(wire, 999);
            frames.insert(frames.end(), rest.begin(), rest.end());
            return frames;
        };
        audioQueueClear(out.queue);
        const auto expected = continuation(out, link);
        assert(expected.size() == 882);
        SavestateLoader<ChunkType> loader;
        assert(loader.Load(saved.data(), saved.size()*4, state));
        assert(out.mixer.Load(savedMixer));
        audioQueueClear(out.queue);
        assert(continuation(out, link) == expected);
        // A new codec/controller and host queue continue both producers and the pending FIFO.
        auto *freshLink = pxaI2sInit(nullptr, nullptr, nullptr);
        auto *freshCodec = ak4534Init(nullptr, freshLink, nullptr);
        MixedOutput fresh;
        ak4534SetOutput(freshCodec, &fresh, MixedOutput::Frame, MixedOutput::Boundary);
        AudioState freshState{freshLink, freshCodec};
        assert(loader.Load(saved.data(), saved.size()*4, freshState));
        assert(fresh.mixer.Load(savedMixer));
        fresh.mixer.SetMuxOutput(true);
        fresh.queue = audioQueueCreate(1024);
        const auto actual = continuation(fresh, freshLink);
        assert(actual == expected && audioQueuePendingSamples(fresh.queue) == expected.size());
        std::vector<uint32_t> queued(actual.size());
        assert(audioQueuePopChunk(fresh.queue, queued.data(), queued.size()) == queued.size());
        assert(queued == expected);
        const auto beforeRate = fresh.mixer.GetState();
        const auto generation = audioQueueGetState(fresh.queue).generation;
        reg(5, ((fs + 1) % 8) << 5);
        const auto afterRate = fresh.mixer.GetState();
        assert(afterRate.dacCount == 0 && afterRate.pwmPhase == beforeRate.pwmPhase &&
               afterRate.hostPhase == beforeRate.hostPhase);
        assert(audioQueueGetState(fresh.queue).generation > generation &&
               audioQueuePendingSamples(fresh.queue) == 0);
        ak4534Destroy(freshCodec);
        ak4534Destroy(ak);
    }
}

static void adcLoopbackChecks() {
    // Reg 5 bit 1 sends the ADC input to the DAC path; with it clear the input never reaches the output.
    for (unsigned loop = 0; loop < 2; ++loop) {
        auto *link = pxaI2sInit(nullptr, nullptr, nullptr);
        auto *ak = ak4534Init(nullptr, link, nullptr);
        reg(0, 0x81); reg(1, 0x61); reg(4, 0x6a); reg(5, loop ? 2 : 0); reg(6, 0x10); reg(7, 0);
        wr(4, 8); wr(0, 0x7701);
        struct Capture { bool nonzero{}; unsigned pulls{}; } capture;
        AudioInputSource source{&capture, [](void *c, AudioInputRoute, uint32_t, uint32_t) -> int16_t {
            static_cast<Capture *>(c)->pulls++; return (static_cast<Capture *>(c)->pulls & 1) ? 12000 : -12000;
        }, nullptr, nullptr};
        ak4534SetInputSource(ak, &source);
        ak4534SetOutput(ak, &capture, [](void *c, uint32_t frame) {
            static_cast<Capture *>(c)->nonzero |= frame != 0;
        }, nullptr);
        for (unsigned i = 0; i < 3000; ++i) { wr(0x80, 0); pxaI2sPeriodic(link); }
        assert(capture.pulls > 2500);
        assert(capture.nonzero == bool(loop));
        ak4534Destroy(ak);
    }
}

int main() {
    auto *link = pxaI2sInit(nullptr, nullptr, nullptr);
    controllerChecks(link); codecChecks(link); mixedChecks(); adcLoopbackChecks();
}
