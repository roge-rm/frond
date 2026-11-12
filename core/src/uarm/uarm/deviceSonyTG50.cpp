//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

// Board wiring adapted from Dmitry Grinberg's uARM deviceSonyTG50.c.
#include "deviceSonyTG50.h"
#include "CsrBcspRadio.h"
#include <memory>

#include <array>
#include <cstdlib>
#include <cstring>

#include "RAM.h"
#include "cputil.h"
#include "i2cdev_AN32502A.h"
#include "i2sdev_AK4534.h"
#include "memory_buffer.h"
#include "mmiodev_MemoryStickController.h"
#include "mmiodev_TG50uc.h"
#include "savestate/savestateAll.h"
#include "sspdev_AD7873.h"
#include "Tg50Chunks.h"
#include "Tg50Audio.h"
#include "Microwire64x16.h"
#include "audio_queue.h"
#include "pxa_PWM.h"
#include "pxa_PwrClk.h"

constexpr uint32_t tg50UcBase = 0x08000000;
constexpr uint32_t tg50UcSize = 0x220;
constexpr uint32_t tg50MsBase = 0x14000000;
constexpr uint32_t tg50MsSize = 0x400;
constexpr uint32_t tg50SramBase = 0xac000000;
constexpr uint32_t tg50SramSize = 1024;

struct Tg50Board {
    CsrBcspRadio* radio{};
    Tg50Audio audio{};
    Microwire64x16 configuration{};
    AudioQueue *queue{};
    PxaPwm *pwm{};
    PxaPwrClk *pwrClk{};
    bool restoringReset{};
    bool jogHeld[2]{};
    ArmMem *mem{};
    PxaGpio *gpio{};
    PxaSsp *ssp{};
    PxaI2c *i2c{};
    Ad7873 *touch{};
    AK4534 *codec{};
    An32502A *pmic{};
    TG50uc *uc{};
    MemoryStickController *ms{};
    MemoryBuffer sramBuffer{};
    ArmRam *sram{};
};

static void tg50ConfigurationOutput(Tg50Board *b) {
    const auto output = b->configuration.DataOut();
    // The undriven board input keeps its existing low idle level.
    pxaGpioSetState(b->gpio, 5, output.driven && output.high);
}
static void tg50ConfigurationPins(void *context) {
    auto *b = static_cast<Tg50Board *>(context);
    const auto select = pxaGpioGetState(b->gpio, 4);
    const auto clock = pxaGpioGetState(b->gpio, 12);
    const auto data = pxaGpioGetState(b->gpio, 5);
    b->configuration.Pins(select == PxaGpioStateHigh, clock == PxaGpioStateHigh,
                          data == PxaGpioStateLow || data == PxaGpioStateHigh,
                          data == PxaGpioStateHigh);
    tg50ConfigurationOutput(b);
}
template <typename H> static void tg50ConfigurationFields(H &h, Microwire64x16::State &s) {
    h.DoBuffer16(s.words.data(), s.words.size());
    h.Do32(s.selected).Do32(s.clock).Do32(s.enabled).Do32(s.phase)
        .Do32(s.commandBits).Do32(s.commandValue).Do32(s.address).Do32(s.readBit)
        .Do32(s.dataBits).Do32(s.data).Do32(s.queued).Do32(s.polling)
        .Do32(s.pending).Do32(s.pendingAddress).Do32(s.pendingValue).Do64(s.remainingNs);
}

static void tg50AudioRegisters(void *context) {
    auto *b = static_cast<Tg50Board *>(context);
    const auto pwm = pxaPwmRegisters(b->pwm);
    b->audio.WritePwm({pwm.control, pwm.duty, pwm.period});
    b->audio.Clock(pxaPwrClkClockMask(b->pwrClk) & 1);
}
static void tg50AudioBoundary(void *context) {
    auto *b = static_cast<Tg50Board *>(context);
    b->audio.CodecBoundary();
    // Already committed mixed output remains ordered across codec controls.
}
static void tg50AudioDac(void *context, uint32_t frame) {
    auto *b = static_cast<Tg50Board *>(context);
    if (!b->audio.PushDac(frame)) ERR("TG50 DAC mixer overflow");
}
void tg50BoardAudioAdvance(Tg50Board *b, uint64_t ns) {
    b->configuration.Advance(ns);
    tg50ConfigurationOutput(b);
    b->audio.SetMuxOutput(!pxaGpioIsInput(b->gpio, 16) &&
                          pxaGpioAlternateFunction(b->gpio, 16) == 2);
    b->audio.Advance(ns, [b](uint32_t frame) { if (b->queue) audioQueuePush(b->queue, frame); });
}

template <typename H> static void tg50AudioFields(H &h, Tg50Audio::State &s) {
    h.Do32(s.programmed.control).Do32(s.programmed.duty).Do32(s.programmed.period)
        .Do32(s.active.control).Do32(s.active.duty).Do32(s.active.period)
        .Do64(s.pwmPhase).Do64(s.hostPhase)
        .Do32(s.clock).Do32(s.running).Do32(s.stopping).Do32(s.dacCount);
    for (auto &frame : s.dac) h.Do32(frame);
}

static std::array<uint32_t, 64> tg50KeyMap() {
    std::array<uint32_t, 64> keys{};
    const KeyId hard[16] = {
        keyIdPower, keyIdHard1, keyIdHard2, keyInvalid,
        keyIdHard3, keyInvalid, keyIdHard4, keyIdVoice,
        keyIdUp, keyIdDown, keyIdHome, keyIdBack,
        keyInvalid, keyIdJogPress, keyIdGraffiti, keyInvalid,
    };
    for (unsigned i = 0; i < 16; ++i) keys[i] = hard[i];
    // The UI's first column contains modifiers, the UC's first sense bit contains letters.
    for (unsigned row = 2; row < 8; ++row)
        for (unsigned sense = 0; sense < 7; ++sense)
            keys[row * 8 + sense] = tg50Key(row, sense == 0 ? 1 : sense == 1 ? 0 : sense);
    return keys;
}

Tg50Board *tg50BoardSetup(SocPeriphs *sp, Keypad *kp,Reschedule reschedule,GetEmuTime time) {
    auto *board = new Tg50Board{};
    board->mem = sp->mem;
    board->gpio = sp->gpio;
    board->pwm = sp->pwm0;
    board->pwrClk = sp->pwrClk;
    board->ssp = sp->ssp;
    board->i2c = sp->i2c;
    board->touch = ad7873Init(board->ssp, board->gpio, 9);
    board->codec = ak4534Init(board->i2c, sp->i2s, board->gpio);
    ak4534SetOutput(board->codec, board, tg50AudioDac, tg50AudioBoundary);
    pxaPwmSetObserver(board->pwm, board, tg50AudioRegisters);
    pxaPwrClkSetClockObserver(board->pwrClk, board, tg50AudioRegisters);
    board->pmic = an32502aInit(board->i2c);
    const auto keys = tg50KeyMap();
    board->uc = tg50ucInit(board->mem, board->gpio, 14, keys.data(), 13);
    board->ms = msCtrlrInit(board->mem, tg50MsBase,board->gpio,sp->dma,reschedule);
    // MemoryBuffer owns whole pages, only the physical 1 KiB is mapped and saved.
    if (!memoryBufferAllocate(&board->sramBuffer, MEMORY_BUFFER_GRANULARITY))
        ERR("cannot allocate TG50 SRAM");
    board->sram = ramInit(board->mem, sp->soc, tg50SramBase, tg50SramSize,
                         &board->sramBuffer, false);
    ramSetNoFramebuffer(board->sram);
    pxaGpioSetPeripheralObserver(board->gpio, tg50ConfigurationPins, board);
    tg50BoardReset(board, true);
    sp->nand = nullptr;
    sp->btUart = sp->uarts[3];
    board->radio = new CsrBcspRadio(time,board->uc,sp->btUart);
    sp->dbgUart = sp->uarts[0];
    return board;
}

void tg50BoardReset(Tg50Board *board, bool clearSram) {
    if(board->radio)board->radio->Reset();
    board->configuration.ResetPins();
    tg50ConfigurationOutput(board);
    ad7873Reset(board->touch);
    ad7873setVbatt(board->touch, 4200);
    ak4534Reset(board->codec);
    board->audio.Reset();
    tg50AudioRegisters(board);
    an32502aReset(board->pmic);
    msCtrlrReset(board->ms);
    tg50ucReset(board->uc);
    board->jogHeld[0] = board->jogHeld[1] = false;
    pxaGpioSetState(board->gpio, 21, true);
    pxaGpioSetState(board->gpio, 22, true);
    for (unsigned pin : {0u, 1u, 3u, 7u, 10u, 13u}) pxaGpioSetState(board->gpio, pin, true);
    msCtrlrRefresh(board->ms);
    if (clearSram) {
        memset(board->sramBuffer.buffer, 0, tg50SramSize);
        memoryBufferMarkRangeDirty(&board->sramBuffer, 0, tg50SramSize);
    }
}

void tg50BoardTouch(Tg50Board *board, int x, int y) {
    const bool down = x >= 0 && y >= 0;
    ad7873PenInput(board->touch, down ? 3200 - 10 * x : -1, down ? 3200 - 10 * y : -1);
    ad7873Periodic(board->touch);
}
void tg50BoardKey(Tg50Board *board, uint32_t key, bool down) {
    if (key == keyIdHold) {
        tg50ucGpioSetInState(board->uc, 0, 4, !down);
        return;
    }
    if (key == keyIdJogUp || key == keyIdJogDown) {
        const unsigned direction = key == keyIdJogDown;
        const bool wasHeld = board->jogHeld[direction];
        board->jogHeld[direction] = down;
        if (down && !wasHeld) {
            pxaGpioSetState(board->gpio, 22, !direction);
            pxaGpioSetState(board->gpio, 21, true);
            pxaGpioSetState(board->gpio, 21, false);
        } else if (!down && !board->jogHeld[!direction]) {
            pxaGpioSetState(board->gpio, 21, true);
        }
        return;
    }
    tg50ucSetKeyPressed(board->uc, key, down);
}
bool tg50BoardHoldSwitchOn(const Tg50Board *board) {
    return !tg50ucGpioInState(board->uc, 0, 4);
}
bool tg50BoardPanelEnabled(const Tg50Board *board) {
    return tg50ucGpioState(board->uc, 1, 9);
}
void tg50BoardPeriodic(Tg50Board *board) { ad7873Periodic(board->touch);msCtrlrPeriodic(board->ms); }
bool tg50BoardTaskRequired(Tg50Board* b){return msCtrlrTaskRequired(b->ms) || b->configuration.Save().remainingNs;}
std::function<void()> tg50BoardCaptureInput(Tg50Board *board) {
    auto touch = ad7873CaptureInput(board->touch);
    auto keys = tg50ucCaptureInput(board->uc);
    auto radio = board->radio->CaptureInput();
    const bool up = board->jogHeld[0], down = board->jogHeld[1];
    return [board, touch, keys, radio, up, down] {
        touch(); keys();radio();
        board->jogHeld[0] = up;
        board->jogHeld[1] = down;
    };
}

template <typename T> void tg50BoardSave(Tg50Board *board, T &savestate) {
    auto *chunk = savestate.GetChunk(tg50BoardChunk, 0);
    if (!chunk) ERR("cannot allocate TG50 board state");
    SaveChunkHelper h(*chunk);
    h.Do32(tg50PhysicalRamSize).Do32(tg50RamBackingSize).Do32(tg50SramSize)
        .DoBuffer(board->sramBuffer.buffer, tg50SramSize);
    tg50ucSave(board->uc, savestate);
    ad7873Save(board->touch, savestate);
    ak4534Save(board->codec, savestate);
    an32502aSave(board->pmic, savestate);
    msCtrlrSave(board->ms, savestate);
    auto *audioChunk = savestate.GetChunk(ChunkType::sonyTg50Audio, 0);
    if (!audioChunk) ERR("cannot allocate TG50 mixer state");
    SaveChunkHelper audioHelper(*audioChunk);
    auto audioState = board->audio.GetState();
    tg50AudioFields(audioHelper, audioState);
    auto *configurationChunk = savestate.GetChunk(ChunkType::sonyTg50Configuration, 0);
    if (!configurationChunk) ERR("cannot allocate TG50 serial configuration state");
    SaveChunkHelper configurationHelper(*configurationChunk);
    auto configurationState = board->configuration.Save();
    tg50ConfigurationFields(configurationHelper, configurationState);
    auto* radioChunk=savestate.GetChunk(ChunkType::sonyTg50Bluetooth,0);
    if(!radioChunk)ERR("cannot allocate TG50 Bluetooth state");
    SaveChunkHelper radioHelper(*radioChunk);auto radioState=board->radio->Save();
    radioHelper.Do32(radioState.size()).DoBuffer(radioState.data(),radioState.size());
}

template <typename T> bool tg50BoardLoad(Tg50Board *board, T &loader, bool preserveInput) {
    auto *chunk = loader.GetChunkOrFail(tg50BoardChunk, 0, "TG50 board");
    if (!chunk) return false;
    uint32_t physical{}, backing{}, sramSize{};
    std::array<uint8_t, tg50SramSize> sram{};
    LoadChunkHelper h(*chunk);
    h.Do32(physical).Do32(backing).Do32(sramSize).DoBuffer(sram.data(), sram.size());
    if (chunk->HasError() || physical != tg50PhysicalRamSize ||
        backing != tg50RamBackingSize || sramSize != tg50SramSize) {
        loader.NotifyError();
        return false;
    }
    Microwire64x16 configuration;
    const bool hasConfiguration = loader.GetChunk(ChunkType::sonyTg50Configuration) != nullptr;
    if (!board->restoringReset && hasConfiguration) {
        auto *configurationChunk = loader.GetChunkOrFail(ChunkType::sonyTg50Configuration, 0,
                                                       "TG50 serial configuration");
        if (!configurationChunk) return false;
        auto candidate = configuration.Save();
        LoadChunkHelper configurationHelper(*configurationChunk);
        tg50ConfigurationFields(configurationHelper, candidate);
        if (!configurationChunk->IsAtEnd() || !configuration.Load(candidate) ||
            bool(candidate.selected) != (pxaGpioGetState(board->gpio, 4) == PxaGpioStateHigh) ||
            bool(candidate.clock) != (pxaGpioGetState(board->gpio, 12) == PxaGpioStateHigh)) {
            loader.NotifyError(); return false;
        }
    }
    if (!tg50ucLoad(board->uc, loader, preserveInput) ||
        !ad7873Load(board->touch, loader, preserveInput) ||
        !ak4534Load(board->codec, loader, preserveInput) ||
        !an32502aLoad(board->pmic, loader, preserveInput) ||
        !(board->restoringReset ? (msCtrlrReset(board->ms),true) : msCtrlrLoad(board->ms, loader, preserveInput))) return false;
    csr::BcspTransport::Bytes radioState;
    if(loader.GetChunk(ChunkType::sonyTg50Bluetooth)){
        auto* radioChunk=loader.GetChunkOrFail(ChunkType::sonyTg50Bluetooth,0,"TG50 Bluetooth");
        if(!radioChunk)return false;
        uint32_t size=0;LoadChunkHelper radioHelper(*radioChunk);radioHelper.Do32(size);
        if(size>8196){loader.NotifyError();return false;}
        radioState.resize(size);radioHelper.DoBuffer(radioState.data(),size);
        if(radioChunk->HasError() || !radioChunk->IsAtEnd()){loader.NotifyError();return false;}
    }
    if(!board->radio->Load(radioState)){loader.NotifyError();return false;}
    if (!board->restoringReset) {
        memcpy(board->sramBuffer.buffer, sram.data(), sram.size());
        memoryBufferMarkRangeDirty(&board->sramBuffer, 0, tg50SramSize);
    }
    Tg50Audio::State audioState{};
    if (loader.GetChunk(ChunkType::sonyTg50Audio)) {
        auto *audioChunk = loader.GetChunkOrFail(ChunkType::sonyTg50Audio, 0, "TG50 mixer");
        if (!audioChunk) return false;
        LoadChunkHelper audioHelper(*audioChunk);
        tg50AudioFields(audioHelper, audioState);
        const auto pwm = pxaPwmRegisters(board->pwm);
        if (audioChunk->HasError() || !Tg50Audio::Valid(audioState) ||
            bool(audioState.clock) != bool(pxaPwrClkClockMask(board->pwrClk) & 1) ||
            audioState.programmed.control != (pwm.control & 0x7f) ||
            audioState.programmed.duty != (pwm.duty & 0x7ff) ||
            audioState.programmed.period != (pwm.period & 0x3ff)) {
            loader.NotifyError(); return false;
        }
        board->audio.Load(audioState);
    } else {
        // Older TG50 sessions start an empty host timeline from the saved PWM registers.
        board->audio.Reset();
        tg50AudioRegisters(board);
    }
    if (board->queue) audioQueueClear(board->queue);
    if (!preserveInput) board->jogHeld[0] = board->jogHeld[1] = false;
    if (board->restoringReset) board->configuration.ResetPins();
    else board->configuration = configuration;
    if (!board->restoringReset && !hasConfiguration) tg50ConfigurationPins(board);
    else tg50ConfigurationOutput(board);
    // GPIO and interrupt-controller state must be loaded before these outputs.
    tg50ucRefreshIrq(board->uc);
    msCtrlrRefresh(board->ms);
    ad7873Periodic(board->touch);
    return true;
}

void tg50BoardDestroy(Tg50Board *board) {
    if (!board) return;
    delete board->radio;board->radio=nullptr;
    // The SoC is stopped; removing each callback prevents a stale context in the retained buses.
    if (!pxaSspRemoveClient(board->ssp, board->touch) ||
        !pxaI2cDeviceRemove(board->i2c, board->codec) ||
        !pxaI2cDeviceRemove(board->i2c, board->pmic) ||
        !memRegionRemove(board->mem, tg50UcBase, tg50UcSize, board->uc) ||
        !memRegionRemove(board->mem, tg50MsBase, tg50MsSize, board->ms) ||
        !memRegionRemove(board->mem, tg50SramBase, tg50SramSize, board->sram))
        ERR("cannot detach TG50 bus callbacks");
    pxaGpioSetPeripheralObserver(board->gpio, nullptr, nullptr);
    pxaPwmSetObserver(board->pwm, nullptr, nullptr);
    pxaPwrClkSetClockObserver(board->pwrClk, nullptr, nullptr);
    ad7873Destroy(board->touch);
    ak4534Destroy(board->codec);
    an32502aDestroy(board->pmic);
    tg50ucDestroy(board->uc);
    msCtrlrDestroy(board->ms);
    free(board->sram);
    memoryBufferRelease(&board->sramBuffer);
    delete board;
}

template void tg50BoardSave<Savestate<ChunkType>>(Tg50Board *, Savestate<ChunkType> &);
template void tg50BoardSave<SavestateProbe<ChunkType>>(Tg50Board *, SavestateProbe<ChunkType> &);
template bool tg50BoardLoad<SavestateLoader<ChunkType>>(Tg50Board *, SavestateLoader<ChunkType> &, bool);

void tg50BoardBeginReset(Tg50Board *board) { board->restoringReset = true; }
void tg50BoardEndReset(Tg50Board *board) { board->restoringReset = false; }

void tg50BoardSetAudioQueue(Tg50Board *board, AudioQueue *queue) {
    if (board->queue == queue) return;
    if (board->queue) audioQueueClear(board->queue);
    board->queue = queue;
    if (queue) audioQueueClear(queue);
}

void tg50BoardAudioSleepReset(Tg50Board *board) {
    // The external codec keeps its registers; the reset PXA domains discard the old stream.
    ak4534ResetOutput(board->codec);
    board->audio.Reset();
    tg50AudioRegisters(board);
}

bool tg50BoardIrPowerEnabled(Tg50Board *board) {
    return tg50ucGpioIsOutput(board->uc, 0, 10) && tg50ucGpioState(board->uc, 0, 10) &&
           pxaGpioGetState(board->gpio, 27) == PxaGpioStateLow;
}
void tg50BoardSetTransportObserver(Tg50Board *board, void (*observer)(void *), void *context) {
    const uint16_t masks[4] = {0x0441, 0x0018, 0, 0};
    tg50ucSetOutputObserver(board->uc, masks, observer, context);
}

void tg50BoardSetUsbCable(Tg50Board *board, bool connected) {
    // AD7873 AUX, 2.5 V reference: 2000 mV gives code3276 in the USB accessory band.
    ad7873setVaux(board->touch, connected ? 2000 : 0);
}
bool tg50BoardUsbCableAttached(Tg50Board *board) {
    const auto code = ad7873getVaux(board->touch);
    return code >= 3152 && code <= 3535;
}
bool tg50BoardUsbListening(Tg50Board *board) {
    return tg50ucGpioIsOutput(board->uc, 1, 3) && !tg50ucGpioState(board->uc, 1, 3) &&
           tg50ucGpioIsOutput(board->uc, 1, 4) && tg50ucGpioState(board->uc, 1, 4);
}

MemoryStickController* tg50BoardMemoryStick(Tg50Board* b){return b->ms;}

void tg50BoardSetInputSource(Tg50Board *b, const AudioInputSource *s) { ak4534SetInputSource(b->codec, s); }
void tg50BoardFenceInput(Tg50Board *b, bool fence) { ak4534FenceInput(b->codec, fence); }
void tg50BoardDisarmInput(Tg50Board *b) { ak4534DisarmInput(b->codec); }

AudioInputDemand tg50BoardInputDemand(Tg50Board *b) { return ak4534InputDemand(b->codec); }

CsrBcspRadio* tg50BoardBluetooth(Tg50Board* board){return board?board->radio:nullptr;}
