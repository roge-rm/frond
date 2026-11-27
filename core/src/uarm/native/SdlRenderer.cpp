#include "SdlRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "SDL_image.h"
#include "SDL_render.h"
#include "Silkscreen.h"
#include "DeviceControls.h"

namespace {
    SDL_Texture* loadSilkscreen(SDL_Renderer* renderer) {
        SDL_RWops* rwops = SDL_RWFromConstMem((const void*)silkscreenPng_data, silkscreenPng_len);
        SDL_Surface* surface = IMG_LoadPNG_RW(rwops);
        SDL_RWclose(rwops);

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);

        SDL_FreeSurface(surface);

        return texture;
    }
}  // namespace

SdlRenderer::SdlRenderer(SDL_Window* window, SoC* soc, float scale,
                         DisplayConfiguration& displayConfiguration, Rotation rotation, bool smoothLcd,
                         WindowFit fit, uint32_t background, DeviceControlView controlView)
    : window(window),
      soc(soc),
      deviceType(soc->GetDeviceType()),
      quarterTurn(soc->FrameQuarterTurn()),
      // A fitted window draws in device pixels and lets the renderer scale and place them.
      scale(fit == WindowFit::none ? scale : 1.f),
      rotation(rotation),
      displayConfiguration(displayConfiguration),
      fit(fit),
      background(background),
      controlView(controlView) {
    int windowHeight, windowWidth;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) return;

    int renderHeight = windowHeight, renderWidth = windowWidth;
    SDL_GetRendererOutputSize(renderer, &renderWidth, &renderHeight);

    if (renderHeight != windowHeight || renderWidth != windowWidth) {
        SDL_RenderSetScale(renderer, static_cast<float>(renderWidth) / windowWidth,
                           static_cast<float>(renderHeight) / windowHeight);
    }

    artworkScale = scale * static_cast<float>(renderWidth) / windowWidth;

    if (fit != WindowFit::none) {
        const bool landscape = rotation == Rotation::landscape_90 || rotation == Rotation::landscape_270;
        const int screenHeight = displayConfiguration.height + displayConfiguration.graffitiHeight;
        const auto size = deviceControlSize(deviceControls(deviceType, controlView),
            landscape ? screenHeight : displayConfiguration.width,
            landscape ? displayConfiguration.width : screenHeight, controlView);
        layout = fitWindowLayout(fit, size.x, size.y, windowWidth, windowHeight);

        // The viewport is in output pixels while the scale is 1, then the scale maps device
        // pixels into it.
        const float pixelsX = static_cast<float>(renderWidth) / windowWidth;
        const float pixelsY = static_cast<float>(renderHeight) / windowHeight;
        SDL_RenderSetScale(renderer, 1, 1);
        const SDL_Rect viewport{int(std::lround(layout.x * pixelsX)), int(std::lround(layout.y * pixelsY)),
                                int(std::lround(layout.width * pixelsX)),
                                int(std::lround(layout.height * pixelsY))};
        SDL_RenderSetViewport(renderer, &viewport);
        SDL_RenderSetScale(renderer, layout.scaleX * pixelsX, layout.scaleY * pixelsY);
        artworkScale = std::max(layout.scaleX * pixelsX, layout.scaleY * pixelsY);
    } else {
        layout = WindowLayout{scale, scale, 0, 0, windowWidth, windowHeight};
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xff);
    SDL_RenderClear(renderer);

    frameTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET,
                                     displayConfiguration.width, displayConfiguration.height);

    intermediateTexture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                          quarterTurn ? displayConfiguration.height : displayConfiguration.width,
                          quarterTurn ? displayConfiguration.width : displayConfiguration.height);

    SDL_SetTextureScaleMode(frameTexture, smoothLcd ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_SetTextureScaleMode(intermediateTexture, SDL_ScaleModeNearest);

    if (displayConfiguration.graffitiHeight > 0) {
        // Devices with drawn silkscreen art use it when it fits the display mode.
        const auto& silkscreen = deviceSilkscreen(deviceType);
        silkscreenTexture = silkscreen.count && silkscreen.width == displayConfiguration.width * silkscreen.unit &&
                                    silkscreen.height == displayConfiguration.graffitiHeight * silkscreen.unit
                                ? createDeviceSilkscreen(renderer, silkscreen, artworkScale)
                                : loadSilkscreen(renderer);
        // Fitted art meets fractional scales.
        if (silkscreenTexture && fit != WindowFit::none)
            SDL_SetTextureScaleMode(silkscreenTexture, SDL_ScaleModeLinear);
    }

    ClearWindow();
    DrawSilkscreen();
    SDL_RenderPresent(renderer);
}

SdlRenderer::~SdlRenderer() {
    SDL_DestroyTexture(frameTexture);
    SDL_DestroyTexture(intermediateTexture);
    SDL_DestroyTexture(silkscreenTexture);
    SDL_DestroyTexture(controlsTexture);
    SDL_DestroyTexture(sidebarTexture);
    SDL_DestroyRenderer(renderer);
}

bool SdlRenderer::SaveFramebuffer(const char* path, bool& blank) {
    blank = !frameTextureValid || !lcdEnabled;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, displayConfiguration.width, displayConfiguration.height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    bool success = true;
    if (blank) {
        success = SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 0, 0, 0, 0xff)) == 0;
    } else {
        SDL_Texture* previous = SDL_GetRenderTarget(renderer);
        success = SDL_SetRenderTarget(renderer, frameTexture) == 0;
        if (success)
            success = SDL_RenderReadPixels(renderer, nullptr, surface->format->format,
                                           surface->pixels, surface->pitch) == 0;
        SDL_SetRenderTarget(renderer, previous);
    }
    if (success) success = IMG_SavePNG(surface, path) == 0;
    SDL_FreeSurface(surface);
    return success;
}

void SdlRenderer::Draw(bool forceRedraw, DeviceKeyMask buttons, DeviceStick stick) {
    const bool lcd = soc->LcdEnabled();

    uint32_t firstDirtyLine=0, lastDirtyLine=0;
    uint32_t* frame = soc->GetPendingFrame(firstDirtyLine, lastDirtyLine);

    if (!gate.Pass(frame != nullptr, forceRedraw, buttons, lcd, stick)) return;
    Render(frame, firstDirtyLine, lastDirtyLine, lcd, buttons, stick);

    soc->ResetPendingFrame();
}

void SdlRenderer::Render(const uint32_t* frame, uint32_t firstDirtyLine, uint32_t lastDirtyLine,
                         bool lcdEnabled, DeviceKeyMask buttons, DeviceStick stick) {
    if (!renderer) return;
    const bool buttonsChanged = pressedButtons != buttons;
    const bool controlsChanged = buttonsChanged || shownStick != stick;
    pressedButtons = buttons;
    shownStick = stick;
    this->lcdEnabled = lcdEnabled;

    if (frame) {
        const int frameWidth = quarterTurn ? displayConfiguration.height : displayConfiguration.width;
        frame += firstDirtyLine * frameWidth;
        uint8_t* pixels;
        int pitch;
        SDL_LockTexture(intermediateTexture, NULL, (void**)&pixels, &pitch);

        pixels += firstDirtyLine * pitch;

        if (pitch == 4 * frameWidth) {
            memcpy(pixels, frame, 4 * frameWidth * (lastDirtyLine - firstDirtyLine + 1));
        } else {
            for (uint32_t y = firstDirtyLine; y <= lastDirtyLine; y++) {
                memcpy(pixels, frame, 4 * frameWidth);
                frame += frameWidth;
                pixels += pitch;
            }
        }

        SDL_UnlockTexture(intermediateTexture);

        SDL_Rect rect = {.x = 0,
                         .y = static_cast<int>(firstDirtyLine),
                         .w = frameWidth,
                         .h = static_cast<int>(lastDirtyLine - firstDirtyLine + 1)};

        if (!frameTextureValid) {
            SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, 0xff);
            SDL_RenderClear(renderer);
        }

        SDL_SetRenderTarget(renderer, frameTexture);
        if (quarterTurn) {
            // The panel's rows become the frame's columns, right to left: turned a quarter
            // clockwise about the centre of a panel-shaped rect in the middle of the frame.
            const int width = displayConfiguration.width, height = displayConfiguration.height;
            const SDL_Rect panel = {.x = (width - height) / 2, .y = (height - width) / 2, .w = height, .h = width};
            SDL_RenderCopyEx(renderer, intermediateTexture, nullptr, &panel, 90, nullptr, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(renderer, intermediateTexture, &rect, &rect, 0, nullptr, SDL_FLIP_NONE);
        }
        SDL_SetRenderTarget(renderer, nullptr);

        frameTextureValid = true;
    }

    ClearWindow();
    DrawSilkscreen();

    if (frameTextureValid && lcdEnabled) {
        // The jog tab turned over the screen moves it down; that only comes with the upright frame.
        const auto screen = ScreenRect();
        SDL_Rect dest = {.x = screen.x,
                         .y = screen.y,
                         .w = int(scale * displayConfiguration.width),
                         .h = int(scale * displayConfiguration.height)};

        if (controlView.zaurus && rotation != Rotation::portrait_0) {
            // The SDL software renderer scales the viewport offset again on a rotated copy.
            // Use output pixels and turn the panel around the centre of the LCD opening.
            float sx, sy;
            SDL_RenderGetScale(renderer, &sx, &sy);
            const float w = screen.w * sx, h = screen.h * sy;
            const bool quarter = rotation == Rotation::landscape_90 || rotation == Rotation::landscape_270;
            const float rawWidth = quarter ? h : w, rawHeight = quarter ? w : h;
            const SDL_FRect panel{screen.x * sx + (w - rawWidth) * 0.5f,
                                  screen.y * sy + (h - rawHeight) * 0.5f, rawWidth, rawHeight};
            SDL_RenderSetScale(renderer, 1, 1);
            SDL_RenderCopyExF(renderer, frameTexture, nullptr, &panel, -90 * int(rotation), nullptr, SDL_FLIP_NONE);
            SDL_RenderSetScale(renderer, sx, sy);
        } else {
            SDL_Point origin;

            switch (rotation) {
                case Rotation::landscape_90:
                    dest.y = int(scale * displayConfiguration.width);
                    origin = {0, 0};
                    SDL_RenderCopyEx(renderer, frameTexture, nullptr, &dest, 270, &origin,
                                     SDL_FLIP_NONE);

                    break;

                case Rotation::portrait_180:
                    dest.y = int(scale * displayConfiguration.graffitiHeight);

                    SDL_RenderCopyEx(renderer, frameTexture, nullptr, &dest, 180, nullptr,
                                     SDL_FLIP_NONE);

                    break;

                case Rotation::landscape_270:
                    dest.x += int(scale * (displayConfiguration.height +
                                      displayConfiguration.graffitiHeight - displayConfiguration.width));
                    dest.y = int(scale * displayConfiguration.width);

                    origin = {int(scale * displayConfiguration.width), 0};

                    SDL_RenderCopyEx(renderer, frameTexture, nullptr, &dest, 90, &origin,
                                     SDL_FLIP_NONE);

                    break;

                default:

                    SDL_RenderCopy(renderer, frameTexture, nullptr, &dest);

                    break;
            }
        }
    }

    const bool landscape=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270;
    const int screenWidth=landscape ? displayConfiguration.height+displayConfiguration.graffitiHeight : displayConfiguration.width;
    const int screenHeight=landscape ? displayConfiguration.width : displayConfiguration.height+displayConfiguration.graffitiHeight;
    if (controlView.zaurus && !controlView.bare) {
        DrawZaurus(buttons, buttonsChanged);
        const auto layout = zaurusDeviceLayout(screenWidth, screenHeight, *controlView.zaurus, scale);
        const auto& body = *controlView.zaurus == ZaurusStyle::view ? layout.side : layout.keyboard;
        const int radius = std::max(1, int(3 * scale));
        for (int i = 0; i < 2; ++i) {
            const SDL_Rect light{body.x + int((24 + i * 12) * scale), body.y + body.h - int(12 * scale),
                                 2 * radius, radius};
            if (stick.lamp & (1 << i))
                SDL_SetRenderDrawColor(renderer, i == 0 ? 255 : 133, i == 0 ? 158 : 235, 62, 255);
            else SDL_SetRenderDrawColor(renderer, 75, 78, 82, 255);
            SDL_RenderFillRect(renderer, &light);
        }
    }
    const auto& controls=deviceControls(deviceType,controlView);
    if(controls.count) {
        // Controls all beside the screen leave nothing to draw under it.
        if(!deviceControlsUnits(controls,controlView)) {
            DrawSidebar(controls,screenWidth,screenHeight,buttons,stick,controlsChanged);
            SDL_RenderPresent(renderer);
            return;
        }
        if(!controlsTexture)
            controlsTexture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_TARGET,int(deviceControlsWidth(controls,screenWidth,controlView)*artworkScale),int(deviceControlsHeight(controls,screenWidth,controlView)*artworkScale));
        if(controlsTexture && fit!=WindowFit::none) SDL_SetTextureScaleMode(controlsTexture,SDL_ScaleModeLinear);
        if(controlsTexture && (!controlsPainted || controlsChanged)) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
            if(SDL_SetRenderTarget(renderer,controlsTexture)==0) {
                SDL_RenderSetScale(renderer,1,1);
                // Transparent where the strip leaves room for the jog panel beside the buttons.
                SDL_SetRenderDrawColor(renderer,0,0,0,0);
                SDL_RenderClear(renderer);
                drawDeviceControls(renderer,controls,screenWidth,0,artworkScale,buttons,controlView,stick);
                SDL_SetRenderTarget(renderer,nullptr);
                SDL_RenderSetScale(renderer,sx,sy);
                controlsPainted=true;
            }
        }
        if(controlsPainted) {
            SDL_Rect dest{0,int(screenHeight*scale),int(deviceControlsWidth(controls,screenWidth,controlView)*scale),int(deviceControlsHeight(controls,screenWidth,controlView)*scale)};
            SDL_RenderCopy(renderer,controlsTexture,nullptr,&dest);
        } else drawDeviceControls(renderer,controls,screenWidth,screenHeight,scale,buttons,controlView,stick);
        // A jog panel beside the screen or the buttons is drawn straight on the window, after the strip
        // so the strip's texture can't cover the part beside the buttons.
        if(controls.sideTabWidth>0) DrawSidebar(controls,screenWidth,screenHeight,buttons,stick,controlsChanged);
    }
    SDL_RenderPresent(renderer);
}

SDL_Rect SdlRenderer::ScreenRect() const {
    const bool landscape = rotation == Rotation::landscape_90 || rotation == Rotation::landscape_270;
    const int height = displayConfiguration.height + displayConfiguration.graffitiHeight;
    const int width = displayConfiguration.width;
    const auto& controls = deviceControls(deviceType, controlView);
    if (controlView.zaurus && !controlView.bare)
        return zaurusDeviceLayout(landscape ? height : width, landscape ? width : height, *controlView.zaurus, scale).screen;
    return {int(scale * deviceScreenLeft(controls, controlView)), int(scale * deviceScreenTop(controls, controlView)),
            int(scale * (landscape ? height : width)), int(scale * (landscape ? width : height))};
}

void SdlRenderer::DrawZaurus(DeviceKeyMask buttons, bool changed) {
    const bool landscape = rotation == Rotation::landscape_90 || rotation == Rotation::landscape_270;
    const int width = landscape ? displayConfiguration.height : displayConfiguration.width;
    const int height = landscape ? displayConfiguration.width : displayConfiguration.height;
    const auto size = deviceControlSize(deviceControls(deviceType, controlView), width, height, controlView);
    if (!controlsTexture) {
        controlsTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, size.x, size.y);
        if (controlsTexture) {
            SDL_SetTextureBlendMode(controlsTexture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(controlsTexture, SDL_ScaleModeLinear);
        }
    }
    if (!controlsPainted || changed) {
        ZaurusKeysHeld held;
        const auto& art = zaurusArt();
        for (size_t i = 0; i < art.count; ++i) {
            const KeyId key = zaurusArtKey(art.keys[i]);
            held.Set(i, key && (buttons & deviceKeyBit(key)));
        }
        if (!controlsTexture) {
            drawZaurusControls(renderer, zaurusDeviceLayout(width, height, *controlView.zaurus, scale), held);
            return;
        }
        float sx, sy;
        SDL_RenderGetScale(renderer, &sx, &sy);
        if (SDL_SetRenderTarget(renderer, controlsTexture) == 0) {
            SDL_RenderSetScale(renderer, 1, 1);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            drawZaurusControls(renderer, zaurusDeviceLayout(width, height, *controlView.zaurus), held);
            SDL_SetRenderTarget(renderer, nullptr);
            SDL_RenderSetScale(renderer, sx, sy);
            controlsPainted = true;
        }
    }
    if (controlsPainted) {
        const SDL_Rect dest{0, 0, int(scale * size.x), int(scale * size.y)};
        SDL_RenderCopy(renderer, controlsTexture, nullptr, &dest);
    }
}

// The panels beside the screen, painted into a viewport-sized texture only when a key, the stick or the LED
// changes, and copied over the window on every other draw. Straight on the window if the texture can't be made.
void SdlRenderer::DrawSidebar(const DeviceControlProfile& controls, int screenWidth, int screenHeight,
                              DeviceKeyMask buttons, DeviceStick stick, bool changed) {
    // The texture covers the viewport in output pixels (the whole window when there is no fit viewport).
    int w = 0, h = 0;
    {
        float vsx, vsy;
        SDL_RenderGetScale(renderer, &vsx, &vsy);
        SDL_RenderSetScale(renderer, 1, 1);
        SDL_Rect viewport{};
        SDL_RenderGetViewport(renderer, &viewport);
        SDL_RenderSetScale(renderer, vsx, vsy);
        w = viewport.w;
        h = viewport.h;
        if (w <= 0 || h <= 0) w = h = 0;
    }
    if (sidebarTexture && (w != sidebarWidth || h != sidebarHeight)) {
        SDL_DestroyTexture(sidebarTexture);
        sidebarTexture = nullptr;
    }
    if (!sidebarTexture && w && h) {
        sidebarTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (sidebarTexture) SDL_SetTextureBlendMode(sidebarTexture, SDL_BLENDMODE_BLEND);
        sidebarWidth = w;
        sidebarHeight = h;
        sidebarPainted = false;
    }
    if (!sidebarTexture) {
        drawDeviceSidebar(renderer, controls, screenWidth, screenHeight, scale, buttons, controlView, stick);
        return;
    }
    if (!sidebarPainted || changed) {
        float sx, sy;
        SDL_RenderGetScale(renderer, &sx, &sy);
        if (SDL_SetRenderTarget(renderer, sidebarTexture) != 0) {
            drawDeviceSidebar(renderer, controls, screenWidth, screenHeight, scale, buttons, controlView, stick);
            return;
        }
        SDL_RenderSetScale(renderer, sx, sy);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        drawDeviceSidebar(renderer, controls, screenWidth, screenHeight, scale, buttons, controlView, stick);
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_RenderSetScale(renderer, sx, sy);
        sidebarPainted = true;
    }
    float sx, sy;
    SDL_RenderGetScale(renderer, &sx, &sy);
    SDL_RenderSetScale(renderer, 1, 1);
    const SDL_Rect dest{0, 0, sidebarWidth, sidebarHeight};
    SDL_RenderCopy(renderer, sidebarTexture, nullptr, &dest);
    SDL_RenderSetScale(renderer, sx, sy);
}

// Clear the visible panel on every draw, including power transitions.
void SdlRenderer::ClearWindow() {
    if (fit == WindowFit::none && !controlView.zaurus) {
        const uint8_t panel = lcdEnabled ? 0xff : 0;
        SDL_SetRenderDrawColor(renderer, panel, panel, panel, 0xff);
        SDL_RenderClear(renderer);
        return;
    }
    SDL_SetRenderDrawColor(renderer, background >> 16, (background >> 8) & 0xff, background & 0xff, 0xff);
    SDL_RenderClear(renderer);
    const auto screen = ScreenRect();
    const uint8_t panel = lcdEnabled ? 0xff : 0;
    SDL_SetRenderDrawColor(renderer, panel, panel, panel, 0xff);
    SDL_RenderFillRect(renderer, &screen);
}

void SdlRenderer::DrawSilkscreen() {
    if (!silkscreenTexture) return;

    SDL_Rect dest = {.x = 0,
                     .y = 0,
                     .w = int(scale * displayConfiguration.width),
                     .h = int(scale * displayConfiguration.graffitiHeight)};

    SDL_Point origin;

    switch (rotation) {
        case Rotation::landscape_90:
            dest.x = int(scale * displayConfiguration.height) - int(scale * displayConfiguration.width);
            origin = {int(scale * displayConfiguration.width), 0};

            SDL_RenderCopyEx(renderer, silkscreenTexture, nullptr, &dest, 270, &origin,
                             SDL_FLIP_NONE);

            break;

        case Rotation::portrait_180:
            SDL_RenderCopyEx(renderer, silkscreenTexture, nullptr, &dest, 180, nullptr,
                             SDL_FLIP_NONE);

            break;

        case Rotation::landscape_270:
            dest.x = int(scale * displayConfiguration.graffitiHeight);
            origin = {0, 0};

            SDL_RenderCopyEx(renderer, silkscreenTexture, nullptr, &dest, 90, &origin,
                             SDL_FLIP_NONE);

            break;

        default:
            dest.y = int(scale * displayConfiguration.height);
            SDL_RenderCopy(renderer, silkscreenTexture, nullptr, &dest);

            break;
    }
}
