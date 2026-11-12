//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _AC97_WM9712L_H_
#define _AC97_WM9712L_H_

#include <functional>

#include <cstdint>

#include "audio_input.h"
#include "get_emu_time.h"
#include "pxa_AC97.h"
#include "pxa_GPIO.h"

struct WM9712L;

struct AudioQueue;

enum WM9712LauxPin {  // Vref is 3.3v,
    WM9712LauxPinAux1 =
        0,  // keep in mind this is divided by 3, so if battery is 3V, pass 1V to this
    WM9712LauxPinAux2,
    WM9712LauxPinBmon,
    WM9712LauxPinAux4,
};

struct WM9712L *wm9712LInit(struct PxaAC97 *ac97, struct PxaGpio *gpio, int8_t penDownPin,
                          int8_t irqPin = -1);
void wm9712Lperiodic(struct WM9712L *wm);

void wm9712LsetAuxVoltage(struct WM9712L *wm, enum WM9712LauxPin which, uint32_t mV);
void wm9712LsetPen(struct WM9712L *wm, int16_t x, int16_t y,
                   int16_t press);  // raw ADC values, negative for pen up

void wm9712LsetAudioQueue(struct WM9712L *wm, struct AudioQueue *audioQueue);

// Boards with a microphone (the Zire 72) get a modelled ADC paced by guest time. Without it the
// PCM-in and mic slots read zero once per PCM period, as before, and the state chunk stays at
// version 0. Host input follows audio_input.h; all calls run on the emulator thread.
void wm9712LEnableAdc(struct WM9712L *wm, struct GetEmuTime getTime);
AudioInputDemand wm9712LInputDemand(struct WM9712L *wm);
void wm9712LSetInputSource(struct WM9712L *wm, const AudioInputSource *source);
void wm9712LFenceInput(struct WM9712L *wm, bool fence);
void wm9712LDisarmInput(struct WM9712L *wm);

template <typename T>
void wm9712Lsave(struct WM9712L *wm, T &savestate);

template <typename T>
void wm9712Load(struct WM9712L *wm, T &loader, bool preserveInput = false);
std::function<void()> wm9712CaptureInput(struct WM9712L *wm);

#endif
