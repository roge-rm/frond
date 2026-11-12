//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _AK4534_H_
#define _AK4534_H_

#include <stdbool.h>
#include <stdint.h>

#include "pxa_I2C.h"
#include "pxa_I2S.h"
#include "pxa_GPIO.h"
#include "audio_input.h"


struct AK4534;



struct AK4534* ak4534Init(struct PxaI2c *i2c, struct PxaI2s *i2s, struct PxaGpio *gpio);




// Host input follows audio_input.h. Units at unity gain are 0.6*AVDD Vpp at ADC peak,
// before MGAIN/IPGA. The internal mic needs MPWRI.
AudioInputDemand ak4534InputDemand(AK4534 *);
void ak4534SetInputSource(AK4534 *, const AudioInputSource *);
void ak4534FenceInput(AK4534 *, bool);
void ak4534DisarmInput(AK4534 *);

struct AudioQueue;
// Synchronous qualification tap. Callers must not mutate the model from the callback.
enum class Ak4534Tap { serialTx, digitalDac, hostOutput };
using Ak4534Observe = void (*)(void *, Ak4534Tap, uint32_t packed,
                              uint32_t rateNumerator, uint32_t rateDenominator);
using Ak4534Output = void (*)(void *, uint32_t);
void ak4534SetOutput(AK4534 *, void *, Ak4534Output, void (*boundary)(void *));
void ak4534SetObserver(AK4534 *, void *, Ak4534Observe);
void ak4534SetAudioQueue(AK4534 *, AudioQueue *);
// Test-only serial input injection, never connected to a host microphone.
void ak4534TestInput(AK4534 *, bool enabled, uint32_t frame);
void ak4534ResetOutput(AK4534 *);
void ak4534Reset(AK4534 *);
// Destroy only after the owning SoC can no longer dispatch its bus callbacks.
void ak4534Destroy(AK4534 *);
template <typename T> void ak4534Save(AK4534 *, T &);
// Candidate loading has no host effects; the SoC disarms only on commit.
template <typename T> bool ak4534Load(AK4534 *, T &, bool preserveInput = false);
#endif
