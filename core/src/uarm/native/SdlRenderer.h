#ifndef _SDL_REDERER_
#define _SDL_REDERER_

#include <SDL.h>

#include <cstdint>

#include "RenderThread.h"
#include "DeviceControls.h"
#include "Rotation.h"
#include "WindowLayout.h"
#include "SoC.h"
#include "device_configuration.h"

class SdlRenderer {
   public:
    SdlRenderer(SDL_Window* window, SoC* soc, float scale, DisplayConfiguration& displayConfiguration,
                Rotation rotation, bool smoothLcd = false, WindowFit fit = WindowFit::none,
                uint32_t background = 0, DeviceControlView controlView = {});
    ~SdlRenderer();

    // Draws the SoC's pending frame on the calling thread, which owns the SoC.
    void Draw(bool forceRedraw, DeviceKeyMask buttons = 0, DeviceStick stick = {});
    // Draws a frame handed over from the guest thread; never touches the SoC. frame is a
    // whole LCD frame, or null to keep the last one; only its dirty rows are uploaded.
    void Render(const uint32_t* frame, uint32_t firstDirtyLine, uint32_t lastDirtyLine, bool lcdEnabled,
                DeviceKeyMask buttons, DeviceStick stick = {});
    // Saves the unrotated LCD frame as PNG. With the LCD off, or before the first frame,
    // it saves a black frame of the same size and sets blank.
    bool SaveFramebuffer(const char* path, bool& blank);
    // Where the device sits in the window, in window coordinates.
    const WindowLayout& Layout() const { return layout; }
    // False when SDL couldn't make the renderer, e.g. with no surface yet. Drawing then
    // does nothing.
    bool Valid() const { return renderer != nullptr; }

   private:
    void DrawSilkscreen();
    void ClearWindow();

   private:
    SDL_Window* window{nullptr};
    SDL_Renderer* renderer{nullptr};

    SDL_Texture* frameTexture{nullptr};
    SDL_Texture* intermediateTexture{nullptr};
    SDL_Texture* silkscreenTexture{nullptr};
    SDL_Texture* controlsTexture{nullptr};
    bool controlsPainted{false};

    bool frameTextureValid{false};
    bool lcdEnabled{true};

    DrawGate gate;
    DeviceKeyMask pressedButtons{0};
    DeviceStick shownStick{};

    SoC* soc{nullptr};
    DeviceType5 deviceType;
    // The frame comes as the panel is wired and the GPU turns it into frameTexture (the Zodiac).
    const bool quarterTurn;

    const float scale;
    float artworkScale{1};
    Rotation rotation;
    DisplayConfiguration displayConfiguration;
    WindowFit fit;
    WindowLayout layout;
    uint32_t background;
    const DeviceControlView controlView;

   private:
    SdlRenderer();
    SdlRenderer(const SdlRenderer&);
    SdlRenderer(SdlRenderer&&);
    SdlRenderer& operator=(const SdlRenderer&);
    SdlRenderer& operator=(SdlRenderer&&);
};

#endif  // _SDL_REDERER_
