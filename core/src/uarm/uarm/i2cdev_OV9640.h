#ifndef _I2CDEV_OV9640_H_
#define _I2CDEV_OV9640_H_

#include <cstdint>

#include "pxa_GPIO.h"
#include "pxa_I2C.h"

// OmniVision OV9640 1.3 MP sensor on the PXA standard I2C (SCCB, 7-bit address 0x30).
// It answers only while powered, out of reset and not powered down. All calls run on the
// emulator thread.
struct Ov9640;

struct Ov9640Pins {
    uint8_t power;     // sensor supply switch
    bool powerActiveHigh;  // level of power that turns the supply on
    uint8_t reset;     // RESET, active high: high holds the registers at their defaults
    uint8_t powerDown; // PWDN, active high: high stops the sensor, registers are kept
};

// What the sensor puts on its 8-bit data bus.
enum class Ov9640Format : uint8_t { yuv422, rgb565, rgb555, rawBayer };

struct Ov9640Output {
    Ov9640Format format;
    uint8_t yuvOrder;     // TSLB bits 3:2: 0 YUYV, 1 YVYU, 2 UYVY, 3 VYUY (bus byte order)
    uint16_t width, height;  // native output size from COM7 and COM1
    uint32_t frameNs;     // frame period at the current input clock, in guest ns
};

struct Ov9640 *ov9640Init(struct PxaI2c *i2c, struct PxaGpio *gpio, Ov9640Pins pins);
void ov9640Destroy(struct Ov9640 *);
// True when the sensor drives its bus: powered, out of reset, PWDN low.
bool ov9640Running(const struct Ov9640 *);
uint8_t ov9640Register(const struct Ov9640 *, uint8_t reg);
// mclkHz is the input clock the QCI gives the sensor (0 when off).
Ov9640Output ov9640Output(const struct Ov9640 *, uint32_t mclkHz);

template <typename T> void ov9640Save(struct Ov9640 *, T &savestate);
// Sessions from before the camera have no sensor chunk; the sensor then starts from power-on.
template <typename T> bool ov9640Load(struct Ov9640 *, T &loader);

#endif
