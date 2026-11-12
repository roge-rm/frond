//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "ac97dev_WM9712L.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_queue.h"
#include "cputil.h"
#include "savestate/savestateAll.h"

// Version 1 adds the ADC model (Zire 72 only); boards without it keep writing version 0.
#define SAVESTATE_VERSION 0
#define SAVESTATE_VERSION_ADC 1

enum WM9712REG {
    RESET = 0x00,
    OUT2VOL = 0x02,
    HPVOL = 0x04,
    MONOOUTVOL = 0x06,
    DACTONECTL = 0x08,
    PCBEEPIN = 0x0a,
    PHONEVOL = 0x0c,
    MICVOL = 0x0e,
    LENEINVOL = 0x10,
    AUXDACVOL = 0x12,
    SIDETONEVOL = 0x14,
    OUT3VOL = 0x16,
    DACVOL = 0x18,
    RECSEL = 0x1a,
    RECGAIN = 0x1c,
    GENPURPOSE = 0x20,
    DEC3DCTL = 0x22,
    PDOWN1 = 0x24,
    PDOWN2 = 0x26,
    EXTDAUDIOID = 0x28,
    EXTDAUDIOCTL = 0x2a,
    DACRATE = 0x2c,
    AUXDACRATE = 0x2e,
    ADCRATE = 0x32,
    SPDIFCTL = 0x3a,
    GPIOCFG = 0x4c,
    GPIOPOLTYP = 0x4e,
    GPIOSTICKY = 0x50,
    GPIOWAKE = 0x52,
    GPIOSTA = 0x54,
    GPIOSHARE = 0x56,
    ADDFUNC1 = 0x58,
    VENDORTEST = 0x5a,
    ADDFUNC2 = 0x5c,
    ALCCTL = 0x60,
    ALCNOISEGATE = 0x62,
    AUXDACINOUTCTL = 0x64,
    DIGI1 = 0x76,
    DIGI2 = 0x78,
    DIGI_RES = 0x7a,
    VID1 = 0x7c,
    VID2 = 0x7e,
};

enum WM9712LsampleIdx {
    WM9712LsampleIdxNone = 0,
    WM9712LsampleIdxX,
    WM9712LsampleIdxY,
    WM9712LsampleIdxPressure,
    WM9712LsampleIdxAux1,
    WM9712LsampleIdxAux2,
    WM9712LsampleIdxBmon,
    WM9712LsampleIdxAux3,
};

struct WM9712L {
    struct PxaGpio *gpio;
    struct PxaAC97 *ac97;
    int8_t penDownPin;
    int8_t irqPin;

    uint16_t digiRegs[3];
    uint16_t addFunc1, vendorTest, addFunc2, pdown1, pdown2, extdCtl, recSel;

    // gpio
    uint16_t gpioCfg, gpioPolTyp, gpioSticky, gpioWake, gpioStatus, gpioSharing;

    // rates
    uint16_t dacRate, auxDacRate, adcRate;

    // volumes
    uint16_t volOut2, volHP, volMono, volPhone, volMic, volOut3, volLineIn, dacVol, recGain,
        volSidetone;

    // external stimuli
    uint16_t vAux[4];  // indexed by enum WM9705auxPin
    uint16_t penX, penY, penZ;
    bool penDown;

    // for state machine
    bool haveUnreadPenData;
    uint8_t cooIdx, numUnreadDatas;
    uint16_t otherTwo[2];

    struct AudioQueue *audioQueue;

    // ADC model, on boards with a microphone. Guest-time pacing: adcClock is the guest ns of the
    // last PCM period, adcPhase the leftover ns*rate below one frame (always < 1e9).
    bool adcModel;
    struct GetEmuTime getTime;
    AudioInputSource input;
    unsigned inputFences;
    uint16_t alcCtl, alcNoiseGate;  // 0x60 and 0x62
    bool adcClockValid;
    uint64_t adcClock, adcPhase;
    // ALC: PGA index in 0.75 dB steps (0 is -17.25 dB, 23 is 0 dB, 63 is +30 dB), the output peak
    // since the last step, frames since the last step and frames since the peak last went over.
    uint8_t alcGain;
    uint16_t alcPeak;
    uint32_t alcElapsed, alcSinceOver;

    template <typename T>
    void DoSaveLoadAdc(T &chunkHelper) {
        chunkHelper.Do(typename T::Pack16() << alcCtl << alcNoiseGate)
            .DoBool(adcClockValid)
            .Do64(adcClock)
            .Do64(adcPhase)
            .Do(typename T::Pack8() << alcGain)
            .Do16(alcPeak)
            .Do32(alcElapsed)
            .Do32(alcSinceOver);
    }

    template <typename T>
    void DoSaveLoad(T &chunkHelper) {
        chunkHelper.DoBuffer16(digiRegs, sizeof(digiRegs) >> 1)
            .Do(typename T::Pack16() << addFunc1 << vendorTest)
            .Do(typename T::Pack16() << addFunc2 << pdown1)
            .Do(typename T::Pack16() << pdown2 << extdCtl)
            .Do(typename T::Pack16() << recSel << gpioCfg)
            .Do(typename T::Pack16() << gpioPolTyp << gpioSticky)
            .Do(typename T::Pack16() << gpioWake << gpioStatus)
            .Do(typename T::Pack16() << gpioSharing << dacRate)
            .Do(typename T::Pack16() << auxDacRate << adcRate)
            .Do(typename T::Pack16() << volOut2 << volHP)
            .Do(typename T::Pack16() << volMono << volPhone)
            .Do(typename T::Pack16() << volMic << volOut3)
            .Do(typename T::Pack16() << volLineIn << dacVol)
            .Do(typename T::Pack16() << recGain << volSidetone)
            .DoBuffer16(vAux, sizeof(vAux) >> 1)
            .DoBool(haveUnreadPenData)
            .Do(typename T::Pack8() << cooIdx << numUnreadDatas)
            .Do(typename T::Pack16() << otherTwo[0] << otherTwo[1]);
    }
};

static void wm9712LprvGpioRecalc(struct WM9712L *wm) {
    // PRP=01 arms automatic digitiser wake-up on pen down (WM9712L table 26).
    if (wm->penDown && (wm->digiRegs[1] & 0xc000) == 0x4000)
        wm->digiRegs[1] |= 0x8000;
    // GPIO13 is the virtual pen detector. Non-sticky inputs reflect their level;
    // sticky inputs are latched on a new active pen transition in setPen().
    constexpr uint16_t pen = 1u << 13;
    const bool active = wm->penDown == bool(wm->gpioPolTyp & pen);
    if (!(wm->gpioSticky & pen)) {
        wm->gpioStatus = (wm->gpioStatus & ~pen) | (active ? pen : 0);
    }
    if (wm->penDownPin >= 0)
        pxaGpioSetState(wm->gpio, wm->penDownPin,
                       !(wm->gpioSharing & 8) && !(wm->gpioCfg & 8) && wm->penDown);
    if (wm->irqPin >= 0) {
        const bool enabled = !(wm->gpioSharing & 4) && !(wm->gpioCfg & 4);
        const bool irq = (wm->gpioStatus & wm->gpioWake) != 0;
        pxaGpioSetState(wm->gpio, wm->irqPin, enabled && (irq != bool(wm->addFunc1 & 1)));
    }
}

static bool wm9712LprvCodecRegR(void *userData, uint32_t regAddr, uint16_t *regValP) {
    struct WM9712L *wm = (struct WM9712L *)userData;
    enum WM9712REG which = (enum WM9712REG)regAddr;
    uint16_t val;

    if (wm->adcModel && (which == ALCCTL || which == ALCNOISEGATE)) {
        *regValP = which == ALCCTL ? wm->alcCtl : wm->alcNoiseGate;
        return true;
    }

    switch (which) {
        case RESET:
            val = 0x6174;
            break;

        case OUT2VOL:
            val = wm->volOut2;
            break;

        case HPVOL:
            val = wm->volHP;
            break;

        case MONOOUTVOL:
            val = wm->volMono;
            break;

        case PHONEVOL:
            val = wm->volPhone;
            break;

        case MICVOL:
            val = wm->volMic;
            break;

        case LENEINVOL:
            val = wm->volLineIn;
            break;

        case SIDETONEVOL:
            val = wm->volSidetone;
            break;

        case OUT3VOL:
            val = wm->volOut3;
            break;

        case DACVOL:
            val = wm->dacVol;
            break;

        case RECSEL:
            val = wm->recSel;
            break;

        case RECGAIN:
            val = wm->recGain;
            break;

        case PDOWN1:
            val = wm->pdown1;
            break;

        case PDOWN2:
            // Read-only ready flags follow PR0/PR1/PR3/PR2 (WM9712L table 41).
            // This model has no analogue settling delay.
            val = (wm->pdown2 & 0x7f00) |
                  ((~wm->pdown2 >> 8) & 0x03) |
                  ((~wm->pdown2 >> 9) & 0x04) |
                  ((~wm->pdown2 >> 7) & 0x08);
            break;

        case EXTDAUDIOID:
            val = 0x0405;
            break;

        case EXTDAUDIOCTL:
            val = wm->extdCtl;
            break;

        case DACRATE:
            val = wm->dacRate;
            break;

        case AUXDACRATE:
            val = wm->auxDacRate;
            break;

        case ADCRATE:
            val = wm->adcRate;
            break;

        case GPIOCFG:
            val = wm->gpioCfg;
            break;

        case GPIOPOLTYP:
            val = wm->gpioPolTyp;
            break;

        case GPIOSTICKY:
            val = wm->gpioSticky;
            break;

        case GPIOWAKE:
            val = wm->gpioWake;
            break;

        case GPIOSTA:
            val = wm->gpioStatus;
            break;

        case GPIOSHARE:
            val = wm->gpioSharing;
            break;

        case ADDFUNC1:
            val = wm->addFunc1;
            break;

        case VENDORTEST:
            val = wm->vendorTest;
            break;

        case ADDFUNC2:
            val = wm->addFunc2;
            break;

        case DIGI1:
            val = wm->digiRegs[0];
            break;

        case DIGI2:
            val = wm->digiRegs[1];
            break;

        case DIGI_RES:
            val = (wm->digiRegs[2] & 0x7fff) | (wm->penDown ? 0x8000 : 0);
            wm->haveUnreadPenData = false;
            // fprintf(stderr, "touch reply (reg) 0x%04x (%s %u %u)\n", val, (val & 0x8000) ? "DN" :
            // "UP", (val >> 12) & 7, val & 0xfff);
            break;

        case VID1:
            val = 0x574D;
            break;
        case VID2:
            val = 0x4C12;
            break;

        // Registers nothing here models read their WM9712L reset values; writes are dropped.
        case DACTONECTL:
            val = 0x0f0f;
            break;

        case PCBEEPIN:
        case AUXDACVOL:
            val = 0xaaa0;
            break;

        case GENPURPOSE:
        case DEC3DCTL:
            val = 0;
            break;

        case SPDIFCTL:
            val = 0x2000;
            break;

        case ALCCTL:
            val = 0xb032;
            break;

        case ALCNOISEGATE:
            val = 0x3e00;
            break;

        default:
            fprintf(stderr, "unknown reg read [0x%04x]\n", (unsigned)regAddr);
            return false;
    }

    // fprintf(stderr, "codec read [0x%04x] -> %04x\n", (unsigned)regAddr, (unsigned)val);
    *regValP = val;
    return true;
}

static bool wm9712LprvCodecRegW(void *userData, uint32_t regAddr, uint16_t val) {
    struct WM9712L *wm = (struct WM9712L *)userData;
    enum WM9712REG which = (enum WM9712REG)regAddr;

    // fprintf(stderr, "codec wri [0x%04x] -> [0x%04x]\n", (unsigned)val, (unsigned)regAddr);

    if (wm->adcModel && (which == ALCCTL || which == ALCNOISEGATE)) {
        (which == ALCCTL ? wm->alcCtl : wm->alcNoiseGate) = val;
        return true;
    }

    switch (which) {
        case OUT2VOL:
            wm->volOut2 = val;
            break;

        case HPVOL:
            wm->volHP = val;
            break;

        case MONOOUTVOL:
            wm->volMono = val;
            break;

        case PHONEVOL:
            wm->volPhone = val;
            break;

        case MICVOL:
            wm->volMic = val;
            break;

        case LENEINVOL:
            wm->volLineIn = val;
            break;

        case SIDETONEVOL:
            wm->volSidetone = val;
            break;

        case OUT3VOL:
            wm->volOut3 = val;
            break;

        case DACVOL:
            wm->dacVol = val;
            break;

        case RECSEL:
            wm->recSel = val;
            break;

        case RECGAIN:
            wm->recGain = val;
            break;

        case PDOWN1:
            wm->pdown1 = val;
            break;

        case PDOWN2:
            wm->pdown2 = val & 0x7f00;
            break;

        case EXTDAUDIOCTL:
            wm->extdCtl = val;
            break;

        case DACRATE:
            wm->dacRate = val;
            break;

        case AUXDACRATE:
            wm->auxDacRate = val;
            break;

        case ADCRATE:
            wm->adcRate = val;
            break;

        case GPIOCFG:
            wm->gpioCfg = val;
            break;

        case GPIOPOLTYP:
            wm->gpioPolTyp = val;
            break;

        case GPIOSTICKY:
            wm->gpioSticky = val;
            break;

        case GPIOWAKE:
            wm->gpioWake = val;
            break;

        case GPIOSTA:
            // Writing zero acknowledges sticky events; writing one does not set them.
            wm->gpioStatus &= val;
            break;

        case GPIOSHARE:
            wm->gpioSharing = val;
            wm9712LprvGpioRecalc(wm);
            break;

        case ADDFUNC1:
            wm->addFunc1 = val;
            break;

        case VENDORTEST:
            wm->vendorTest = val;
            break;

        case ADDFUNC2:
            wm->addFunc2 = val;
            break;

        case DIGI1:
            wm->digiRegs[0] = val;
            break;

        case DIGI2:
            wm->digiRegs[1] = val;
            break;

        case DACTONECTL:
        case PCBEEPIN:
        case AUXDACVOL:
        case GENPURPOSE:
        case DEC3DCTL:
        case SPDIFCTL:
        case ALCCTL:
        case ALCNOISEGATE:
            return true;

        default:
            fprintf(stderr, "unknown reg wri [0x%04x] -> [0x%04x]\n", (unsigned)val,
                    (unsigned)regAddr);
            return false;
    }
    wm9712LprvGpioRecalc(wm);
    return true;
}

struct WM9712L *wm9712LInit(struct PxaAC97 *ac97, struct PxaGpio *gpio, int8_t penDownPin,
                          int8_t irqPin) {
    struct WM9712L *wm = (struct WM9712L *)malloc(sizeof(*wm));

    if (!wm) ERR("cannot alloc WM9712L");

    memset(wm, 0, sizeof(*wm));

    wm->penDownPin = penDownPin;
    wm->irqPin = irqPin;
    wm->ac97 = ac97;
    wm->gpio = gpio;

    // misc regs
    wm->addFunc1 = 0x0008;
    wm->extdCtl = 0x0410;

    // gpio regs
    wm->gpioCfg = 0xf83e;
    wm->gpioPolTyp = 0xffff;
    wm->gpioSharing = 0xf83e;
    wm->recSel = 0x3000;

    // volume regs
    wm->volOut2 = 0x8000;
    wm->volHP = 0x8000;
    wm->volMono = 0x8000;
    wm->volPhone = 0xc008;
    wm->volMic = 0x6808;
    wm->volLineIn = 0xe808;
    wm->volSidetone = 0xad00;
    wm->volOut3 = 0x8000;
    wm->dacVol = 0xe808;
    wm->recGain = 0x8000;

    // rates
    wm->dacRate = 0xbb80;
    wm->auxDacRate = 0xbb80;
    wm->adcRate = 0xbb80;

    // Only the primary audio space has a codec; the others get no completion, as before.
    pxaAC97clientAdd(ac97, Ac97PrimaryAudio, wm9712LprvCodecRegR, wm9712LprvCodecRegW, wm);

    return wm;
}

static void wm9712LprvNewAudioPlaybackSample(struct WM9712L *wm, uint32_t samp) {
    if (wm->audioQueue) audioQueuePush(wm->audioQueue, samp);
}

static bool wm9712LprvHaveAudioOutSample(struct WM9712L *wm, uint32_t *sampP) {
    *sampP = 0;
    return true;
}

// ADC power (WM9712L tables 40-41): PR0 in 0x26 powers the ADCs, 0x24 bits 12 and 11 the left
// and right ADC. VRA (0x2a bit 0) sets the rate from ADCRATE; without it the model keeps the
// old one zero sample per PCM period.
static bool wm9712LprvAdcLeft(const struct WM9712L *wm) { return !(wm->pdown1 & 0x1000); }
static bool wm9712LprvAdcRight(const struct WM9712L *wm) { return !(wm->pdown1 & 0x0800); }
static bool wm9712LprvAdcRunning(const struct WM9712L *wm) {
    return wm->adcModel && !(wm->pdown2 & 0x0100) && (wm->extdCtl & 1) && wm->adcRate &&
           (wm9712LprvAdcLeft(wm) || wm9712LprvAdcRight(wm));
}
// RECSEL 0x1a bits 10:8 and 2:0 pick the left and right record source; 0 is the microphone.
// Other sources read silence: the Zire has nothing on them.
static bool wm9712LprvMicLeft(const struct WM9712L *wm) {
    return wm9712LprvAdcLeft(wm) && !(wm->recSel & 0x0700);
}
static bool wm9712LprvMicRight(const struct WM9712L *wm) {
    return wm9712LprvAdcRight(wm) && !(wm->recSel & 0x0007);
}

AudioInputDemand wm9712LInputDemand(struct WM9712L *wm) {
    AudioInputDemand demand;
    demand.route = AudioInputRoute::internalMic;
    demand.fenced = wm->inputFences != 0;
    demand.active = wm9712LprvAdcRunning(wm) && (wm9712LprvMicLeft(wm) || wm9712LprvMicRight(wm));
    if (demand.active) {
        demand.rateNumerator = wm->adcRate;
        demand.rateDenominator = 1;
    }
    return demand;
}

void wm9712LDisarmInput(struct WM9712L *wm) {
    if (wm->input.disarm) wm->input.disarm(wm->input.context);
    wm->input = {};
}

void wm9712LSetInputSource(struct WM9712L *wm, const AudioInputSource *source) {
    if (!wm->adcModel) return;
    wm9712LDisarmInput(wm);
    if (source) wm->input = *source;
    if (wm->inputFences && wm->input.fence) wm->input.fence(wm->input.context, true);
}

void wm9712LFenceInput(struct WM9712L *wm, bool fence) {
    if (!wm->adcModel) return;
    const auto before = wm->inputFences;
    if (fence) ++wm->inputFences;
    else if (wm->inputFences) --wm->inputFences;
    if (bool(before) != bool(wm->inputFences) && wm->input.fence)
        wm->input.fence(wm->input.context, wm->inputFences != 0);
}

void wm9712LEnableAdc(struct WM9712L *wm, struct GetEmuTime getTime) {
    wm->adcModel = true;
    wm->getTime = getTime;
    wm->alcGain = 23;
}

// Static record PGA (0x1c): bit 15 mutes; bits 13:8 and 5:0 are the left and right gain, in
// 0.75 dB steps from -17.25 dB when bit 14 or 6 is set, else 1.5 dB steps from 0 dB up to
// +22.5 dB as on a standard AC'97 codec. Returned as a 0.75 dB index like alcGain.
static uint8_t wm9712LprvPgaIndex(const struct WM9712L *wm, bool right) {
    const unsigned field = right ? wm->recGain & 0x7f : (wm->recGain >> 8) & 0x7f;
    const unsigned gain = field & 0x3f;
    return (field & 0x40) ? gain : 23 + 2 * std::min(gain, 15u);
}

// ALC (0x60, 0x62), an approximation of WM9712L section "Automatic level control": target
// -22.5 dBFS plus 1.4 dB per step up to -1.5 dBFS, maximum PGA gain -12 dB plus 6 dB per step,
// attack 1.05 ms * 2^ATK and decay 4.2 ms * 2^DCY per 0.75 dB step, hold 2.67 ms * 2^(HLD-1).
// The noise gate (0x62 bit 7) holds the gain, or mutes with bit 5, below -76.5 dBFS plus
// 1.5 dB per step of bits 4:0.
static unsigned wm9712LprvAlcChannels(const struct WM9712L *wm) { return wm->alcNoiseGate >> 14; }

static int32_t wm9712LprvApplyGain(int32_t input, unsigned boostDb, uint8_t pga) {
    const double db = double(boostDb) - 17.25 + 0.75 * pga;
    const double value = std::round(input * std::pow(10.0, db / 20.0));
    return int32_t(std::clamp(value, -32768.0, 32767.0));
}

static void wm9712LprvAlcStep(struct WM9712L *wm, int32_t output, bool alcActive) {
    if (!alcActive) {
        // Start from the PGA of the channel the ALC controls: right only (1) seeds from the
        // right, left or both from the left.
        wm->alcGain = wm9712LprvPgaIndex(wm, wm9712LprvAlcChannels(wm) == 1);
        wm->alcPeak = 0;
        wm->alcElapsed = wm->alcSinceOver = 0;
        return;
    }
    const double rate = wm->adcRate;
    const auto frames = [&](double ms) { return std::max<uint32_t>(1, uint32_t(rate * ms / 1000.0)); };
    const uint32_t attack = frames(1.05 * (1u << (wm->alcCtl & 0xf)));
    const uint32_t decay = frames(4.2 * (1u << ((wm->alcCtl >> 4) & 0xf)));
    const unsigned holdBits = (wm->alcCtl >> 8) & 0xf;
    const uint32_t hold = holdBits ? frames(2.67 * (1u << (holdBits - 1))) : 0;
    const uint16_t target = uint16_t(32768.0 * std::pow(10.0, (-22.5 + 1.4 * (wm->alcCtl >> 12)) / 20.0));
    const uint8_t maxGain = uint8_t(std::min(63.0, (-12.0 + 6.0 * ((wm->alcNoiseGate >> 11) & 7) + 17.25) / 0.75));
    const uint16_t magnitude = uint16_t(std::min(32767, std::abs(output)));
    wm->alcPeak = std::max(wm->alcPeak, magnitude);
    ++wm->alcElapsed;
    if (magnitude > target) wm->alcSinceOver = 0;
    else if (wm->alcSinceOver < UINT32_MAX) ++wm->alcSinceOver;
    if (wm->alcPeak > target) {
        if (wm->alcElapsed < attack) return;
        if (wm->alcGain) --wm->alcGain;
    } else {
        if (wm->alcElapsed < decay) return;
        const uint16_t gate = uint16_t(32768.0 * std::pow(10.0, (-76.5 + 1.5 * (wm->alcNoiseGate & 0x1f)) / 20.0));
        const bool gated = (wm->alcNoiseGate & 0x80) && wm->alcPeak < gate;
        if (!gated && wm->alcSinceOver >= hold && wm->alcGain < maxGain) ++wm->alcGain;
        else if (wm->alcGain > maxGain) wm->alcGain = maxGain;
    }
    wm->alcPeak = 0;
    wm->alcElapsed = 0;
}

// One ADC frame: left in bits 15:0, right in bits 31:16 of the PCM-in slot pair.
static uint32_t wm9712LprvAdcFrame(struct WM9712L *wm) {
    const bool left = wm9712LprvMicLeft(wm), right = wm9712LprvMicRight(wm);
    int32_t input = 0;
    if ((left || right) && !wm->inputFences && wm->input.pull)
        input = wm->input.pull(wm->input.context, AudioInputRoute::internalMic, wm->adcRate, 1);
    // Mic preamp boost 0x0e bit 7 and record boost 0x1a bit 14, 20 dB each.
    const unsigned boost = ((wm->volMic & 0x80) ? 20 : 0) + ((wm->recSel & 0x4000) ? 20 : 0);
    const unsigned alc = wm9712LprvAlcChannels(wm);
    const bool alcLeft = alc & 2, alcRight = alc & 1;
    const auto channel = [&](bool on, bool useAlc, bool isRight) -> int32_t {
        if (!on || (wm->recGain & 0x8000)) return 0;
        return wm9712LprvApplyGain(input, boost, useAlc ? wm->alcGain : wm9712LprvPgaIndex(wm, isRight));
    };
    const int32_t l = channel(left, alcLeft, false), r = channel(right, alcRight, true);
    const bool alcActive = (alcLeft && left) || (alcRight && right);
    const int32_t detected = std::max(alcLeft && left ? std::abs(l) : 0, alcRight && right ? std::abs(r) : 0);
    bool gateMute = false;
    if (alcActive && (wm->alcNoiseGate & 0xa0) == 0xa0) {
        const double gate = 32768.0 * std::pow(10.0, (-76.5 + 1.5 * (wm->alcNoiseGate & 0x1f)) / 20.0);
        gateMute = detected < gate;
    }
    wm9712LprvAlcStep(wm, detected, alcActive);
    if (gateMute) return 0;
    return (uint32_t(uint16_t(r)) << 16) | uint16_t(l);
}

// Frames due since the last PCM period at ADCRATE. At most 64 a period (the FIFO holds 16);
// a longer gap, such as the PCM task being held, drops the rest without pulling them.
static void wm9712LprvAdcAdvance(struct WM9712L *wm) {
    const uint64_t now = wm->getTime.getTimeF(wm->getTime.userdata);
    if (!wm->adcClockValid || now < wm->adcClock) {
        wm->adcClockValid = true;
        wm->adcClock = now;
        wm->adcPhase = 0;
        return;
    }
    const uint64_t elapsed = std::min<uint64_t>(now - wm->adcClock, 1000000000ull);
    wm->adcClock = now;
    wm->adcPhase += elapsed * wm->adcRate;
    uint64_t due = wm->adcPhase / 1000000000ull;
    wm->adcPhase %= 1000000000ull;
    if (due > 64) due = 64;
    for (uint64_t i = 0; i < due; ++i)
        pxaAC97clientClientHaveData(wm->ac97, Ac97PrimaryAudio, wm9712LprvAdcFrame(wm));
}

static bool wm9712LprvHaveMicOutSample(struct WM9712L *wm, uint32_t *sampP) {
    *sampP = 0;
    return true;
}

static uint16_t wm9712LprvGetSample(struct WM9712L *wm, enum WM9712LsampleIdx which) {
    uint16_t ret;

    switch (which) {
        case WM9712LsampleIdxNone:
            ret = 0;
            break;

        case WM9712LsampleIdxX:
            ret = wm->penX;
            break;

        case WM9712LsampleIdxY:
            ret = wm->penY;
            break;

        case WM9712LsampleIdxPressure:
            ret = wm->penZ;
            break;

        case WM9712LsampleIdxAux1:
            ret = wm->vAux[WM9712LauxPinAux1];
            break;

        case WM9712LsampleIdxAux2:
            ret = wm->vAux[WM9712LauxPinAux2];
            break;

        case WM9712LsampleIdxBmon:
            ret = wm->vAux[WM9712LauxPinBmon];
            break;

        case WM9712LsampleIdxAux3:
            ret = wm->vAux[WM9712LauxPinAux4];
            break;

        default:
            ret = 0;
            break;
    }

    ret &= 0x0fff;

    ret |= ((uint16_t)which) << 12;

    return ret;
}

static bool wm9712LprvHaveModemOutSample(struct WM9712L *wm, uint32_t *sampP) {
    uint16_t retVal;

    // if we are not in slot mode, and we have unread data, we cannot proceed and bail out early
    if (!(wm->digiRegs[0] & 0x08) && wm->haveUnreadPenData && (wm->digiRegs[1] & 0x0200))
        return false;

    // if we are in slot mode but any slot except 5 is selected, provide no data - pxa255 cnanot get
    // it anyways, plus this is callback for modem data (slot5)
    if ((wm->digiRegs[0] & 0x08) && (wm->digiRegs[0] & 0x07)) return false;

    // Continuous pen conversions require the digitiser to be powered. Auxiliary
    // polling remains available while pen detection is armed for wake-up.
    if (!wm->numUnreadDatas && !(wm->digiRegs[0] & 0x8000) &&
        (wm->digiRegs[1] & 0xc000) != 0xc000) return false;

    // if we have unread data, send it and do nothing else
    if (wm->numUnreadDatas) retVal = wm->otherTwo[2 - wm->numUnreadDatas--];
    // else, if needed do a sampling (poll is set or continuous is set and pdet is off or pen is
    // down
    else if ((wm->digiRegs[0] & 0x8000) ||
             ((wm->digiRegs[0] & 0x0400) && (wm->penDown || !(wm->digiRegs[1] & 0x0800)))) {
        enum WM9712LsampleIdx addrIdx = (enum WM9712LsampleIdx)((wm->digiRegs[0] >> 12) & 7);

        // see if we need a set
        if (wm->digiRegs[0] & 0x0800) {
            uint16_t y;

            // get x
            retVal = wm9712LprvGetSample(wm, WM9712LsampleIdxX);

            // get y
            y = wm9712LprvGetSample(wm, WM9712LsampleIdxY);

            // get third if needed
            if (addrIdx != WM9712LsampleIdxNone) {
                wm->otherTwo[0] = y;
                wm->otherTwo[1] = wm9712LprvGetSample(wm, addrIdx);
                wm->numUnreadDatas = 2;
            } else {
                wm->otherTwo[1] = y;
                wm->numUnreadDatas = 1;
            }
        }
        // else read one thing (even if "none")
        else {
            retVal = wm9712LprvGetSample(wm, addrIdx);
        }
    }
    // else no data
    else
        return false;

    // Slot streaming and register readback expose the same conversion result.
    wm->digiRegs[2] = retVal;
    // A coordinate poll finishes after its final conversion, not after X.
    if (!wm->numUnreadDatas) wm->digiRegs[0] &= ~0x8000;
    // provide ret val (not necessarily in the slot)
    if (wm->digiRegs[0] & 0x08) {
        // fprintf(stderr, "touch reply (slot) 0x%04x (%s %u %u)\n", retVal, (retVal & 0x8000) ?
        // "DN" : "UP", (retVal >> 12) & 7, retVal & 0xfff);
        *sampP = retVal | (wm->penDown ? 0x8000 : 0);
        return true;
    }

    return false;
}

void wm9712Lperiodic(struct WM9712L *wm) {
    uint32_t val;

    if (pxaAC97clientClientWantData(wm->ac97, Ac97PrimaryAudio, &val))
        wm9712LprvNewAudioPlaybackSample(wm, val);

    if (wm9712LprvAdcRunning(wm))
        wm9712LprvAdcAdvance(wm);
    else {
        wm->adcClockValid = false;
        if (wm9712LprvHaveAudioOutSample(wm, &val))
            pxaAC97clientClientHaveData(wm->ac97, Ac97PrimaryAudio, val);
    }

    if (wm9712LprvHaveMicOutSample(wm, &val))
        pxaAC97clientClientHaveData(wm->ac97, Ac97SecondaryAudio, val);

    if (wm9712LprvHaveModemOutSample(wm, &val))
        pxaAC97clientClientHaveData(wm->ac97, Ac97PrimaryModem, val);
}

void wm9712LsetAuxVoltage(struct WM9712L *wm, enum WM9712LauxPin which, uint32_t mV) {
    // vref is 3.3V

    if (mV > 3300)
        mV = 0xfff;
    else
        mV = (mV * 4095 + 3300 / 2) / 3300;

    wm->vAux[which] = mV;
}

void wm9712LsetPen(struct WM9712L *wm, int16_t x, int16_t y,
                   int16_t press)  // raw ADC values, negative for pen up
{
    const bool wasDown = wm->penDown;
    if (x >= 0 && y >= 0 && press >= 0) {
        wm->penX = x;
        wm->penY = y;
        wm->penZ = press;
        wm->penDown = true;
    } else {
        wm->penX = 0x3fff;
        wm->penY = 0x3fff;
        wm->penZ = 0;
        wm->penDown = false;
    }

    constexpr uint16_t pen = 1u << 13;
    if (wm->penDown != wasDown && (wm->gpioSticky & pen) &&
        wm->penDown == bool(wm->gpioPolTyp & pen)) wm->gpioStatus |= pen;
    wm9712LprvGpioRecalc(wm);
}

void wm9712LsetAudioQueue(struct WM9712L *wm, struct AudioQueue *audioQueue) {
    wm->audioQueue = audioQueue;
}

template <typename T>
void wm9712Lsave(struct WM9712L *wm, T &savestate) {
    auto chunk = savestate.GetChunk(ChunkType::wm9712L,
                                    wm->adcModel ? SAVESTATE_VERSION_ADC : SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    wm->DoSaveLoad(helper);
    if (wm->adcModel) wm->DoSaveLoadAdc(helper);
}

std::function<void()> wm9712CaptureInput(struct WM9712L *wm) {
    const auto x = wm->penX, y = wm->penY, z = wm->penZ;
    const bool down = wm->penDown;
    return [wm, x, y, z, down] {
        wm->penX = x; wm->penY = y; wm->penZ = z; wm->penDown = down;
    };
}

template <typename T>
void wm9712Load(struct WM9712L *wm, T &loader, bool preserveInput) {
    uint32_t version = 0;
    auto chunk = loader.GetChunkOrFail(ChunkType::wm9712L,
                                       wm->adcModel ? SAVESTATE_VERSION_ADC : SAVESTATE_VERSION,
                                       "wm9712L", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    wm->DoSaveLoad(helper);
    if (wm->adcModel) {
        // Version 0 has no ADC state: the ALC starts from the register gain and the clock
        // restarts at the next PCM period. Host input is never saved.
        wm->alcCtl = wm->alcNoiseGate = 0;
        wm->adcClockValid = false;
        wm->adcClock = wm->adcPhase = 0;
        wm->alcGain = wm9712LprvPgaIndex(wm, false);
        wm->alcPeak = 0;
        wm->alcElapsed = wm->alcSinceOver = 0;
        if (version >= SAVESTATE_VERSION_ADC) {
            wm->DoSaveLoadAdc(helper);
            if (wm->adcPhase >= 1000000000ull || wm->alcGain > 63 || wm->alcPeak > 32767) {
                loader.NotifyError();
                return;
            }
        }
    }

    if (!preserveInput) wm9712LsetPen(wm, 0, 0, -1);
}

template void wm9712Lsave<Savestate<ChunkType>>(WM9712L *wm, Savestate<ChunkType> &savestate);
template void wm9712Lsave<SavestateProbe<ChunkType>>(WM9712L *wm,
                                                     SavestateProbe<ChunkType> &savestate);
template void wm9712Load<SavestateLoader<ChunkType>>(WM9712L *wm,
                                                     SavestateLoader<ChunkType> &loader, bool);
