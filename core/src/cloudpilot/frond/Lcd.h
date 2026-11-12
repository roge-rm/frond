#ifndef FROND_68K_LCD_H
#define FROND_68K_LCD_H

// The 68k LCD in RGBA: Cloudpilot's 1, 2 or 4 bpp frame as grey levels, coloured like the
// real panel and adjusted by the contrast setting, with the green glow of the EL backlight when
// the guest turns it on. A colour panel's 24-bit frame (the IIIc's SED1375, the m505's SED1376,
// the m130's MediaQ) is shown as it is, and its panel is dark when the LCD is off.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>

#include "EmHAL.h"
#include "EmSession.h"
#include "Frame.h"
#include "Nibbler.h"

class Lcd68k {
   public:
    static constexpr int kWidth = 160, kHeight = 160;
    static constexpr int kDefaultContrast = 50;

    Lcd68k(int width = kWidth, int height = kHeight, bool colour = false)
        : width(width), height(height), colour(colour) {
        if (width <= 0 || width > 1024 || height <= 0 || height > 1024)
            throw std::invalid_argument("invalid 68k LCD dimensions");
        pixels.resize(size_t(width) * height * 4);
        SetContrast(kDefaultContrast);
    }
    int Width() const { return width; }
    int Height() const { return height; }

    // 0 (faint) to 100 (dark); 50 shows the guest's greys as they are.
    void SetContrast(int value) {
        contrast = std::clamp(value, 0, 100);
        for (int level = 0; level < 16; ++level) {
            palette[0][level] = Colour(level, contrast, kPanel, kInk);
            palette[1][level] = Colour(level, contrast, kLitPanel, kLitInk);
        }
    }
    int Contrast() const { return contrast; }

    // Reads the guest frame. False with the LCD off or an unexpected frame size; the pixels
    // then show the unlit panel.
    bool Update() {
        on = gSession->IsPowerOn() && EmHAL::CopyLCDFrame(frame, true) &&
             frame.lineWidth * frame.scaleX == width && frame.lines * frame.scaleY == height &&
             frame.scaleX == 1 && frame.scaleY == 1 &&
             (frame.bpp == 1 || frame.bpp == 2 || frame.bpp == 4 || frame.bpp == 24) &&
             (uint64_t(frame.margin) + width) * (frame.bpp == 24 ? 32 : frame.bpp) <=
                 uint64_t(frame.bytesPerLine) * 8 &&
             uint64_t(frame.bytesPerLine) * height <= frame.GetBufferSize();
        lit = on && EmHAL::GetLCDBacklightOn();
        const Rgb* palette = this->palette[lit ? 1 : 0];
        if (!on) {
            for (int i = 0; i < width * height; ++i) Put(i, colour ? kColourOff : palette[0]);
            return false;
        }
        const uint8_t* buffer = frame.GetBuffer();
        const uint16_t mapping = EmHAL::GetLCD2bitMapping();
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = buffer + y * frame.bytesPerLine;
            uint8_t* out = pixels.data() + y * width * 4;
            switch (frame.bpp) {
                case 1: {
                    Nibbler<1> nibbler;
                    nibbler.reset(row, frame.margin);
                    for (int x = 0; x < width; ++x, out += 4) Copy(out, palette[nibbler.nibble() ? 15 : 0]);
                    break;
                }
                case 2: {
                    Nibbler<2> nibbler;
                    nibbler.reset(row, frame.margin);
                    for (int x = 0; x < width; ++x, out += 4)
                        Copy(out, palette[(mapping >> (4 * nibbler.nibble())) & 0x0f]);
                    break;
                }
                case 4: {
                    Nibbler<4> nibbler;
                    nibbler.reset(row, frame.margin);
                    for (int x = 0; x < width; ++x, out += 4) Copy(out, palette[nibbler.nibble()]);
                    break;
                }
                case 24: {
                    // Cloudpilot's colour pixels are 32-bit 0xAABBGGRR, so R, G, B in memory on a
                    // little-endian host. A grey panel never gives a 24-bit frame; one is shown as grey levels.
                    const uint8_t* in = row + 4 * frame.margin;
                    if (colour)
                        for (int x = 0; x < width; ++x, out += 4, in += 4) Copy(out, Rgb{in[0], in[1], in[2]});
                    else
                        for (int x = 0; x < width; ++x, out += 4, in += 4)
                            Copy(out, palette[15 - (in[0] + in[1] + in[2]) / 48]);
                    break;
                }
            }
        }
        return true;
    }

    bool On() const { return on; }
    bool IsColour() const { return colour; }
    // The backlight as of the last Update.
    bool Lit() const { return lit; }
    // RGBA bytes, Width() x Height().
    const uint8_t* Pixels() const { return pixels.data(); }

   private:
    struct Rgb {
        uint8_t r, g, b;
    };

    // Unlit panel and full ink.
    static constexpr Rgb kPanel{0xc2, 0xcb, 0xb0}, kInk{0x16, 0x1c, 0x1c};
    // Lit by the backlight: the panel glows green, the ink stays dark.
    static constexpr Rgb kLitPanel{0x7e, 0xd8, 0x9a}, kLitInk{0x0e, 0x24, 0x18};
    // A colour panel with the LCD off.
    static constexpr Rgb kColourOff{0x1c, 0x1e, 0x20};

    // Level 0 is the panel, 15 full ink. Lower contrast fades the ink towards the panel; higher
    // contrast darkens the panel towards the ink, as the wheel on the real one does.
    static Rgb Colour(int level, int contrast, const Rgb& panel, const Rgb& full) {
        double ink = level / 15.0;
        if (contrast <= 50) ink *= 0.25 + 0.75 * contrast / 50.0;
        else ink += (1 - ink) * 0.5 * (contrast - 50) / 50.0;
        auto mix = [&](uint8_t from, uint8_t to) { return uint8_t(from + (to - from) * ink + 0.5); };
        return {mix(panel.r, full.r), mix(panel.g, full.g), mix(panel.b, full.b)};
    }

    static void Copy(uint8_t* out, const Rgb& colour) {
        out[0] = colour.r;
        out[1] = colour.g;
        out[2] = colour.b;
        out[3] = 0xff;
    }
    void Put(int index, const Rgb& colour) { Copy(pixels.data() + 4 * index, colour); }

    Frame frame{1024 * 1024};
    Rgb palette[2][16]{};
    int width, height;
    bool colour;
    std::vector<uint8_t> pixels;
    int contrast{kDefaultContrast};
    bool on{false};
    bool lit{false};
};

#endif
