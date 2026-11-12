#ifndef _CAMERA_INPUT_H_
#define _CAMERA_INPUT_H_

#include <cstdint>

// A host picture source for a camera sensor (the Zire 72's OV9640). All calls run on the
// emulator thread. pull fills width x height RGB24 pixels (rows top down, R, G, B bytes) for
// guest frame number frame and returns false when it has no picture, so the core draws its
// test pattern instead; it must never block. fence freezes the source while checkpointed guest
// work runs (the core draws the pattern meanwhile); disarm closes it. Host sources are never
// session state and a fresh start or load has none.
struct CameraSource {
    void *context{};
    bool (*pull)(void *, uint32_t width, uint32_t height, uint64_t frame, uint8_t *rgb){};
    void (*fence)(void *, bool){};
    void (*disarm)(void *){};
};

// The built-in test pattern: eight colour bars over the top 70%, a grey band with a white bar
// that moves one sixty-fourth of the width each frame, and the frame number in white on black
// along the bottom fifth. Laid out in fractions of the size, so every size shows the same picture.
void cameraTestPattern(uint32_t width, uint32_t height, uint64_t frame, uint8_t *rgb);

// The bar colours left to right, as RGB.
extern const uint8_t cameraTestPatternBars[8][3];

#endif
