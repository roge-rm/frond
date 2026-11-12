//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "i2sdev_AK4534.h"
#include <string.h>
#include <stdlib.h>
#include <cmath>
#include <algorithm>
#include <iterator>
#include <new>
#include "audio_queue.h"
#include "cputil.h"
#include "Tg50Chunks.h"
#include "Tg50Trace.h"
#include "savestate/savestateAll.h"




struct AK4534 {
    bool trace{};
    PxaI2s *i2s{};
    AudioQueue *queue{};
    void *outputContext{};
    Ak4534Output output{};
    void (*outputBoundary)(void *){};
    void *observerContext{};
    Ak4534Observe observer{};
    // Guest digital state survives host queue discontinuities.
    uint32_t gain[2]{65536, 65536};
    uint32_t target[2]{65536, 65536};
    uint32_t ramp[2]{};
    uint32_t outPhase{};
    int64_t outSum[2]{};
    uint32_t wireNumerator{}, wireDenominator{};
    uint32_t injectedRx{};
    bool testRx{};
    AudioInputSource input{};
    unsigned inputFences{};
    struct Adc {
        uint32_t startup{}, gain{16}, pending{16}, changeWait{}, recovery{}, limiter{};
        int32_t previous{};
        int64_t hpInput{}, hpOutput{};
        int32_t delay[17]{};
        uint32_t delayAt{};
    } adc;

	
	struct PxaGpio *gpio;
	
	//regs
	uint8_t regs[16];	//last two forced to zero
	
	//i2c state machine state
	struct {
		uint8_t inTransaction;
		uint8_t addrSeen;
		uint8_t ourAddr;
		uint8_t isRead;
		uint8_t regAddrSeen;
		uint8_t reg;
	} i2c;
};

// Manufacturer register map, unused bits read as zero.
static constexpr uint8_t ak4534Masks[] = {0xeb,0xef,0x6f,0x8f,0xff,0xfb,0xff,0x1f,0x7f,0x7f,0x7f,0x7f,0xff,0xff};
static constexpr uint32_t ak4534Rates[] = {44100, 22050, 11025, 48000, 32000, 24000, 16000, 8000};

static uint32_t ak4534Clock(const AK4534 *ak) {
    // Observed TG50 crystal/PLL/MCKO path. Other clock sources need board evidence.
    return (ak->regs[1] & 0xe0) == 0x60 && (ak->regs[4] & 0xc8) == 0x48
        ? ak4534Rates[ak->regs[5] >> 5] : 0;
}

static void ak4534Boundary(void *context) {
    auto *ak = static_cast<AK4534 *>(context);
    if (ak->outputBoundary) ak->outputBoundary(ak->outputContext);
    ak->outPhase = 0;
    ak->outSum[0] = ak->outSum[1] = 0;
    ak->wireNumerator = ak->wireDenominator = 0;
    if (ak->queue) audioQueueClear(ak->queue);
}

static void ak4534Targets(AK4534 *ak) {
    for (unsigned ch = 0; ch < 2; ++ch) {
        const auto code = ak->regs[12 + ((ak->regs[6] & 0x10) ? 0 : ch)];
        const uint32_t target = (code == 255 || (ak->regs[6] & 0x20)) ? 0 :
            static_cast<uint32_t>(std::lround(65536.0 * std::pow(10.0, -double(code) / 40.0)));
        if (target != ak->target[ch]) {
            ak->target[ch] = target;
            // Full-range attenuation takes 1061/fs. SMUTE timing needs the complete datasheet.
            ak->ramp[ch] = 1061;
        }
    }
}

static void ak4534Resample(AK4534 *ak, int32_t left, int32_t right,
                           uint32_t numerator, uint32_t denominator) {
    if (!numerator || !denominator) return;
    if (ak->wireNumerator != numerator || ak->wireDenominator != denominator) {
        ak4534Boundary(ak);
        ak->wireNumerator = numerator;
        ak->wireDenominator = denominator;
    }
    uint32_t remain = 44100 * denominator;
    while (remain) {
        const auto portion = std::min(remain, numerator - ak->outPhase);
        ak->outSum[0] += int64_t(left) * portion;
        ak->outSum[1] += int64_t(right) * portion;
        ak->outPhase += portion;
        remain -= portion;
        if (ak->outPhase == numerator) {
            const auto l = static_cast<int16_t>(ak->outSum[0] / numerator);
            const auto r = static_cast<int16_t>(ak->outSum[1] / numerator);
            const uint32_t packed = uint16_t(l) | (uint32_t(uint16_t(r)) << 16);
            if (ak->observer) ak->observer(ak->observerContext, Ak4534Tap::hostOutput, packed, 44100, 1);
            if (ak->output) ak->output(ak->outputContext, packed);
            else if (ak->queue) audioQueuePush(ak->queue, packed);
            ak->outPhase = 0;
            ak->outSum[0] = ak->outSum[1] = 0;
        }
    }
}

AudioInputDemand ak4534InputDemand(AK4534 *ak) {
    AudioInputDemand demand;
    const bool mic = ak->regs[7] & 4, external = ak->regs[7] & 2;
    demand.route = !mic ? AudioInputRoute::analogIn : external ?
        AudioInputRoute::externalMic : AudioInputRoute::internalMic;
    pxaI2sGetRate(ak->i2s, demand.rateNumerator, demand.rateDenominator);
    demand.fenced = ak->inputFences != 0;
    demand.active = (ak->regs[0] & 0x81) == 0x81 && ak4534Clock(ak) &&
        pxaI2sLinkEnabled(ak->i2s) && demand.rateNumerator && demand.rateDenominator &&
        (!mic || ((ak->regs[0] & 2) && (ak->regs[7] & (external ? 16 : 8))));
    return demand;
}

void ak4534DisarmInput(AK4534 *ak) {
    if (ak->input.disarm) ak->input.disarm(ak->input.context);
    ak->input = {};
}
void ak4534SetInputSource(AK4534 *ak, const AudioInputSource *source) {
    ak4534DisarmInput(ak);
    if (source) ak->input = *source;
    if (ak->inputFences && ak->input.fence) ak->input.fence(ak->input.context, true);
}
void ak4534FenceInput(AK4534 *ak, bool fence) {
    const auto before = ak->inputFences;
    if (fence) ++ak->inputFences;
    else if (ak->inputFences) --ak->inputFences;
    if (bool(before) != bool(ak->inputFences) && ak->input.fence)
        ak->input.fence(ak->input.context, ak->inputFences != 0);
}

static int32_t ak4534Adc(AK4534 *ak, bool msb, uint32_t n, uint32_t d) {
    auto &a = ak->adc;
    const unsigned dif = ak->regs[4] & 3;
    if ((ak->regs[0] & 0x81) != 0x81 || !ak4534Clock(ak) || !n || !d) return 0;
    const bool initialising = a.startup != 0;
    if (a.startup) --a.startup;
    const bool mic = ak->regs[7] & 4;
    const bool external = ak->regs[7] & 2;
    const bool connected = !mic || ((ak->regs[0] & 2) &&
        (ak->regs[7] & (external ? 16 : 8)));
    int32_t input = 0;
    if (connected && !ak->inputFences && ak->input.pull)
        input = ak->input.pull(ak->input.context, !mic ? AudioInputRoute::analogIn :
            external ? AudioInputRoute::externalMic : AudioInputRoute::internalMic, n, d);
    if (mic && (ak->regs[7] & 1)) input *= 10;
    const bool crossing = !input || (input < 0) != (a.previous < 0);
    a.previous = input;
    const auto ztm = 128u << ((ak->regs[8] >> 4) & 3);
    bool completedAttenuation = false;
    if (a.gain != a.pending && (crossing || ++a.changeWait >= ztm)) {
        completedAttenuation = a.pending < a.gain;
        if (completedAttenuation) a.recovery = 0;
        a.gain = a.pending; a.changeWait = 0;
    }
    const auto amplify = [&](unsigned gain) {
        return mic ? int32_t(std::lround(input * std::pow(10.0, (int(gain) - 16) / 40.0))) : input;
    };
    int32_t amplified = amplify(a.gain);
    if (mic && (ak->regs[0] & 2) && (ak->regs[9] & 32)) {
        const unsigned magnitude = std::abs(amplified);
        const unsigned threshold = (ak->regs[9] & 1) ? 20675 : 16423;
        const unsigned reset = (ak->regs[9] & 1) ? 16423 : 13045;
        if (magnitude >= threshold) {
            a.recovery = 0;
            if (a.pending > a.gain) { a.pending = a.gain; a.changeWait = 0; }
            if (ak->regs[9] & 16) {
                // LTM00 permits two half-frame steps, otherwise 1/2/4 frames.
                const unsigned ltm = ak->regs[8] & 3;
                if (!a.limiter) {
                    const unsigned steps = 1 + ((ak->regs[9] >> 2) & 3);
                    const unsigned operations = ltm ? 1 : 2;
                    for (unsigned i = 0; i < operations && unsigned(std::abs(amplify(a.gain))) >= threshold; ++i)
                        a.gain = a.pending = a.gain > steps ? a.gain - steps : 0;
                    a.limiter = ltm ? (1u << (ltm - 1)) : 1;
                }
            } else if (a.pending == a.gain && !completedAttenuation) {
                const unsigned steps = 1 + ((ak->regs[9] >> 2) & 3);
                a.pending = a.gain > steps ? a.gain - steps : 0;
                a.changeWait = 0;
                if (crossing) a.gain = a.pending;
            }
        } else if (magnitude >= reset) a.recovery = 0;
        else if (++a.recovery >= (128u << ((ak->regs[8] >> 2) & 3))) {
            a.recovery = 0;
            if (a.pending == a.gain && a.gain < std::min(71u, unsigned(ak->regs[10]))) {
                a.pending = std::min(std::min(71u, unsigned(ak->regs[10])),
                    a.gain + 1 + ((ak->regs[9] >> 1) & 1));
                a.changeWait = 0;
                if (crossing) a.gain = a.pending;
            }
        }
        if (a.limiter) --a.limiter;
        amplified = amplify(a.gain);
    }
    amplified = std::clamp(amplified, -32768, 32767);
    // One-pole DC cancellation, 3.4Hz at 44.1kHz, fixed coefficient per fs.
    const int64_t x = int64_t(amplified) * 65536;
    constexpr int64_t pole = 1073221810; // round(exp(-2*pi*3.4/44100)*2^30)
    a.hpOutput = ((a.hpOutput + x - a.hpInput) * pole) / (int64_t(1) << 30);
    a.hpInput = x;
    const auto sample = std::clamp(int32_t(a.hpOutput / 65536), -32768, 32767);
    // Datasheet ADC group delay is17/fs; pipeline runs during initialisation.
    const auto delayed = a.delay[a.delayAt];
    a.delay[a.delayAt] = sample;
    a.delayAt = (a.delayAt + 1) % 17;
    return (!initialising && dif < 3 && (msb ? dif < 2 : dif == 2)) ? delayed : 0;
}

static uint32_t ak4534Frame(void *context, uint32_t packed, bool playing,
                            bool msbJustified, uint32_t numerator, uint32_t denominator) {
    auto *ak = static_cast<AK4534 *>(context);
    if (ak->observer) ak->observer(ak->observerContext, Ak4534Tap::serialTx, packed, numerator, denominator);
    const auto adc = ak4534Adc(ak, msbJustified, numerator, denominator);
    // MS0133-E-03 figures6-8: mono SDTO in the left slot, right held low.
    const uint32_t rx = uint16_t(adc);
    if (ak->regs[5] & 2) packed = rx;
    int32_t samples[2] = {static_cast<int16_t>(packed), static_cast<int16_t>(packed >> 16)};
    const unsigned dif = ak->regs[4] & 3;
    // Retained format evidence establishes I2S only. Other wire formats stay silent.
    const bool formatMatches = !msbJustified && dif == 2;
    const bool powered = playing && formatMatches && (ak->regs[0] & 0x80) && (ak->regs[1] & 1);
    for (unsigned ch = 0; ch < 2; ++ch) {
        if (ak->ramp[ch]) {
            const int64_t delta = int64_t(ak->target[ch]) - ak->gain[ch];
            ak->gain[ch] = uint32_t(int64_t(ak->gain[ch]) + delta / ak->ramp[ch]);
            if (!--ak->ramp[ch]) ak->gain[ch] = ak->target[ch];
        }
        // Digital DAC output; physical speaker/HP routing has a separate qualification gate.
        samples[ch] = powered ? int32_t((int64_t(samples[ch]) * ak->gain[ch]) / 65536) : 0;
    }
    if (ak->observer) ak->observer(ak->observerContext, Ak4534Tap::digitalDac,
        uint16_t(samples[0]) | (uint32_t(uint16_t(samples[1])) << 16), numerator, denominator);
    ak4534Resample(ak, samples[0], samples[1], numerator, denominator);
    return ak->testRx ? ak->injectedRx : rx;
}

void ak4534SetAudioQueue(AK4534 *ak, AudioQueue *queue) {
    if (ak->queue == queue) return;
    if (ak->queue) ak4534Boundary(ak);
    ak->queue = queue;
    // First host attachment preserves a restored guest resampler's partial frame.
    if (queue) audioQueueClear(queue);
}

void ak4534TestInput(AK4534 *ak, bool enabled, uint32_t frame) {
    ak->testRx = enabled;
    ak->injectedRx = frame;
}

static bool ak4534prvDoRegWrite(struct AK4534 *ak, uint8_t reg, uint8_t val)
{
	//TG50_TRACE( "AK4534: writing 0x%02x -> [0x%02x]\n", val, reg);
	
	if (reg < sizeof(ak->regs) / sizeof(*ak->regs)) {
        if (reg >= 14) return false;
        const auto oldClock = ak4534Clock(ak);
        const auto old = ak->regs[reg];
        ak->regs[reg] = val & ak4534Masks[reg];
        if (reg == 0) {
            if ((old ^ val) & 1) {
                ak->adc.startup = (val & 1) ? 2081 : 0;
                ak->adc.hpInput = ak->adc.hpOutput = 0;
                std::fill(std::begin(ak->adc.delay), std::end(ak->adc.delay), 0);
                ak->adc.delayAt = 0;
            }
            if (!(val & 2)) {
                ak->adc.gain = ak->adc.pending = 16;
                ak->adc.changeWait = ak->adc.recovery = ak->adc.limiter = 0;
            }
        }
        if (reg == 9 && (old & 32) && !(val & 32)) {
            ak->adc.pending = ak->adc.gain;
            ak->adc.changeWait = ak->adc.recovery = ak->adc.limiter = 0;
        }
        if (reg == 11 && (ak->regs[0] & 2) && !(ak->regs[9] & 32)) {
            ak->adc.pending = std::min(71u, unsigned(ak->regs[11]));
            ak->adc.changeWait = 0;
        }
        if (ak->trace) fprintf(stderr, "TG50 audio register %02x=%02x clock=%u\n", reg, ak->regs[reg], ak4534Clock(ak));
        if (reg == 6 || reg == 12 || reg == 13) ak4534Targets(ak);
        if (ak4534Clock(ak) != oldClock) pxaI2sSetExternalRate(ak->i2s, ak4534Clock(ak));
        if (reg == 0 || reg == 1 || reg == 3 || (reg == 4 && ((old ^ val) & 3)) ||
            (reg == 5 && ((old ^ val) & 2)))
            ak4534Boundary(ak);
        return true;
	}
	
	TG50_TRACE( "UNKNOWN REG WRITTEN  0x%02x -> [0x%02x]\n", val, reg);
	return false;
}

static uint8_t ak4534prvDoRegRead(struct AK4534 *ak, uint8_t reg)
{
	//TG50_TRACE( "AK4534: reading [0x%02x]\n", reg);
	
	if (reg < 14)
		return ak->regs[reg];
	
	TG50_TRACE( "UNKNOWN REG READ: 0x%02x\n", reg);
	return 0;
}

static uint_fast8_t ak4534prvI2cHandler(void *userData, enum ActionI2C stimulus, uint_fast8_t value)
{
	struct AK4534 *ak = (struct AK4534*)userData;
	
	switch (stimulus) {
		case i2cStart:
			ak->i2c.inTransaction = 1;
			ak->i2c.regAddrSeen = 0;
			//fallthrough
		
		case i2cRestart:
			ak->i2c.addrSeen = 0;
			ak->i2c.ourAddr = 0;
			return 0;
		
		case i2cStop:
			ak->i2c.inTransaction = 0;
			return 0;
		
		case i2cTx:
			if (!ak->i2c.inTransaction)
				return 0;
			if (!ak->i2c.addrSeen) {
				ak->i2c.addrSeen = 1;
				if ((value >> 1) != 0x10)
					return 0;
				ak->i2c.ourAddr = 1;
				ak->i2c.isRead = value & 1;
				return 1;
			}
			if (!ak->i2c.ourAddr)
				return 0;
			if (ak->i2c.isRead) {
				
				TG50_TRACE( "unexpected write in read mode\n");
				return 0;
			}
			//write to us
			if (!ak->i2c.regAddrSeen) {
				ak->i2c.regAddrSeen = 1;
				ak->i2c.reg = value;
				
				return 1;
			}
			//write to a reg
			return ak4534prvDoRegWrite(ak, ak->i2c.reg++, value) ? 1 : 0;
		
		case i2cRx:
			if (!ak->i2c.inTransaction || !ak->i2c.addrSeen || !ak->i2c.ourAddr)
				return 0;
			if (!ak->i2c.isRead) {
				
				TG50_TRACE( "unexpected read in write mode\n");
				return 0;
			}
			return ak4534prvDoRegRead(ak, ak->i2c.reg++);
		
		default:
			return 0;
	}
}


void ak4534Reset(AK4534 *ak) {
    ak4534Boundary(ak);
    if (!ak->inputFences) ak4534DisarmInput(ak);
    ak->adc = {};
    ak->testRx = false;
    ak->injectedRx = 0;
    for (unsigned ch = 0; ch < 2; ++ch) {
        ak->gain[ch] = ak->target[ch] = 65536;
        ak->ramp[ch] = 0;
    }
    memset(ak->regs, 0, sizeof(ak->regs));
    ak->i2c = {};
	ak->regs[1] = 0x80;
	ak->regs[3] = 0x03;
	ak->regs[4] = 0x02;
	ak->regs[6] = 0x11;
	ak->regs[7] = 0x01;
	ak->regs[9] = 0x40;
	ak->regs[10] = 0x36;
	ak->regs[11] = 0x10;
    if (ak->i2s) pxaI2sSetExternalRate(ak->i2s, 0);
	
}

struct AK4534* ak4534Init(struct PxaI2c *i2c, struct PxaI2s *i2s, struct PxaGpio *gpio)
{
	struct AK4534 *ak = new (std::nothrow) AK4534{};
	
	if (!ak)
		ERR("cannot alloc AK4534");
	

	
    ak->trace = getenv("FROND_TG50_AUDIO_TRACE") != nullptr;
    ak->i2s = i2s;
    ak->gpio = gpio;
    ak4534Reset(ak);
    pxaI2sAttach(i2s, ak, ak4534Frame, ak4534Boundary);

	if (!pxaI2cDeviceAdd(i2c, ak4534prvI2cHandler, ak))
		ERR("cannot add AK4534 to I2C\n");
	
	return ak;
}

void ak4534Destroy(AK4534 *ak) {
    pxaI2sAttach(ak->i2s, nullptr, nullptr, nullptr);
    ak4534DisarmInput(ak);
    delete ak;
}

template <typename H> static void ak4534State(AK4534 &state, H &h) {
    for (auto &reg : state.regs) h.Do8(reg);
    h.Do8(state.i2c.inTransaction).Do8(state.i2c.addrSeen).Do8(state.i2c.ourAddr).Do8(state.i2c.isRead).Do8(state.i2c.regAddrSeen).Do8(state.i2c.reg);
}
template <typename H> static void ak4534AdcState(AK4534::Adc &a, H &h) {
    h.Do32(a.startup).Do32(a.gain).Do32(a.pending).Do32(a.changeWait)
        .Do32(a.recovery).Do32(a.limiter).Do32(a.previous).Do64(a.hpInput).Do64(a.hpOutput);
    for (auto &sample : a.delay) h.Do32(sample);
    h.Do32(a.delayAt);
}
template <typename T> void ak4534Save(AK4534 *ak, T &savestate) {
    auto *chunk = savestate.GetChunk(tg50CodecChunk, 2);
    if (!chunk) ERR("cannot allocate TG50 model state");
    SaveChunkHelper h(*chunk);
    ak4534State(*ak, h);
    for (unsigned ch = 0; ch < 2; ++ch) h.Do32(ak->gain[ch]).Do32(ak->target[ch]).Do32(ak->ramp[ch]);
    h.Do32(ak->outPhase).Do64(ak->outSum[0]).Do64(ak->outSum[1])
        .Do32(ak->wireNumerator).Do32(ak->wireDenominator);
    ak4534AdcState(ak->adc, h);
}
template <typename T> bool ak4534Load(AK4534 *ak, T &loader, bool preserveInput) {
    uint32_t version{};
    auto *chunk = loader.GetChunkOrFail(tg50CodecChunk, 2, "TG50 ak4534", version);
    if (!chunk) return false;
    auto next = *ak;
    LoadChunkHelper h(*chunk);
    ak4534State(next, h);
    for (unsigned ch = 0; ch < 2; ++ch) {
        next.gain[ch] = next.target[ch] = 65536;
        next.ramp[ch] = 0;
    }
    next.outPhase = 0;
    next.outSum[0] = next.outSum[1] = 0;
    next.wireNumerator = next.wireDenominator = 0;
    if (version >= 1) {
        for (unsigned ch = 0; ch < 2; ++ch) h.Do32(next.gain[ch]).Do32(next.target[ch]).Do32(next.ramp[ch]);
        h.Do32(next.outPhase).Do64(next.outSum[0]).Do64(next.outSum[1])
            .Do32(next.wireNumerator).Do32(next.wireDenominator);
    } else ak4534Targets(&next);
    next.adc = {};
    if (version >= 2) ak4534AdcState(next.adc, h);
    else if (next.regs[0] & 1) next.adc.startup = 2081;
    const auto &a = next.adc;
    for (auto sample : a.delay) if (sample < -32768 || sample > 32767) {
        loader.NotifyError(); return false;
    }
    if (a.delayAt >= 17) { loader.NotifyError(); return false; }
    if (a.startup > 2081 || a.gain > 71 || a.pending > 71 || a.changeWait > 1024 ||
        a.recovery > 1024 || a.limiter > 4 || std::abs(int64_t(a.previous)) > 327680 ||
        a.hpInput < -int64_t(32768)*65536 || a.hpInput > int64_t(32767)*65536 ||
        (a.hpOutput < -int64_t(65536)*65536 || a.hpOutput > int64_t(65536)*65536)) { loader.NotifyError(); return false; }
    const bool wireValid = (!next.wireNumerator && !next.wireDenominator && !next.outPhase &&
        !next.outSum[0] && !next.outSum[1]) ||
        ((next.wireDenominator == 1 && std::find(std::begin(ak4534Rates), std::end(ak4534Rates),
            next.wireNumerator) != std::end(ak4534Rates)) ||
         (next.wireNumerator == 576000 && next.wireDenominator >= 1 && next.wireDenominator <= 127));
    if (!wireValid || (next.wireNumerator && next.outPhase >= next.wireNumerator)) {
        loader.NotifyError(); return false;
    }
    for (unsigned ch = 0; ch < 2; ++ch) {
        if (next.gain[ch] > 65536 || next.target[ch] > 65536 || next.ramp[ch] > 1061 ||
            next.outSum[ch] < -int64_t(32768) * next.outPhase ||
            next.outSum[ch] > int64_t(32767) * next.outPhase) { loader.NotifyError(); return false; }
    }
    if (chunk->HasError()) { loader.NotifyError(); return false; }
    if (next.i2c.inTransaction > 1 || next.i2c.addrSeen > 1 || next.i2c.ourAddr > 1 || next.i2c.isRead > 1 || next.i2c.regAddrSeen > 1) { loader.NotifyError(); return false; }
    for (unsigned reg = 0; reg < 16; ++reg) {
        const auto mask = reg < 14 ? ak4534Masks[reg] : 0;
        if (version && (next.regs[reg] & ~mask)) { loader.NotifyError(); return false; }
        next.regs[reg] &= mask;
    }
    auto expected = next;
    ak4534Targets(&expected);
    for (unsigned ch = 0; ch < 2; ++ch)
        if (next.target[ch] != expected.target[ch]) { loader.NotifyError(); return false; }
    uint32_t numerator{}, denominator{};
    pxaI2sGetRate(ak->i2s, numerator, denominator);
    // Receiver rate is derived from the candidate codec, not the old live registers.
    if (denominator == 1 && numerator != 576000) numerator = ak4534Clock(&next);
    if (next.wireNumerator && (next.wireNumerator != numerator || next.wireDenominator != denominator)) {
        loader.NotifyError(); return false;
    }
    if (next.wireNumerator && !pxaI2sLinkEnabled(ak->i2s)) {
        loader.NotifyError(); return false;
    }
    next.testRx = false;
    next.injectedRx = 0;
    *ak = next;
    if (ak->queue) audioQueueClear(ak->queue);
    // Re-derive clock input without discarding saved controller or resampler phase.
    pxaI2sSetExternalRate(ak->i2s, ak4534Clock(ak), false);
    // Board refreshes output pins after GPIO, IC and all models have loaded.
    return true;
}
template void ak4534Save<Savestate<ChunkType>>(AK4534*, Savestate<ChunkType>&);
template void ak4534Save<SavestateProbe<ChunkType>>(AK4534*, SavestateProbe<ChunkType>&);
template bool ak4534Load<SavestateLoader<ChunkType>>(AK4534*, SavestateLoader<ChunkType>&, bool);

void ak4534SetObserver(AK4534 *ak, void *context, Ak4534Observe observer) {
    ak->observerContext = context;
    ak->observer = observer;
}

void ak4534SetOutput(AK4534 *ak, void *ctx, Ak4534Output output, void (*boundary)(void *)) {
    ak->outputContext = ctx; ak->output = output; ak->outputBoundary = boundary;
}

void ak4534ResetOutput(AK4534 *ak) { ak4534Boundary(ak); }
