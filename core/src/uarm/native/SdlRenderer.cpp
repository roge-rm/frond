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
        const int deviceWidth = deviceControlsWidth(deviceControls(deviceType),
            landscape ? screenHeight : displayConfiguration.width, controlView);
        const int deviceHeight = (landscape ? displayConfiguration.width : screenHeight) +
                                 deviceControlsHeight(deviceControls(deviceType),landscape ? screenHeight : displayConfiguration.width,controlView);
        layout = fitWindowLayout(fit, deviceWidth, deviceHeight, windowWidth, windowHeight);

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
    const bool controlsChanged = pressedButtons != buttons || shownStick != stick;
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
        SDL_Rect dest = {.x = int(scale * deviceScreenLeft(deviceControls(deviceType),controlView)),
                         .y = 0,
                         .w = int(scale * displayConfiguration.width),
                         .h = int(scale * displayConfiguration.height)};

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

    const bool landscape=rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270;
    const int screenWidth=landscape ? displayConfiguration.height+displayConfiguration.graffitiHeight : displayConfiguration.width;
    const int screenHeight=landscape ? displayConfiguration.width : displayConfiguration.height+displayConfiguration.graffitiHeight;
    const auto& controls=deviceControls(deviceType);
    if(controls.count) {
        // Controls all beside the screen leave nothing to draw under it.
        if(!deviceControlsUnits(controls,controlView)) {
            drawDeviceSidebar(renderer,controls,screenWidth,screenHeight,scale,buttons,controlView,stick);
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
        if(controls.sideTabWidth>0) drawDeviceSidebar(renderer,controls,screenWidth,screenHeight,scale,buttons,controlView,stick);
    }
    SDL_RenderPresent(renderer);
}

// Clear the visible panel on every draw, including power transitions.
void SdlRenderer::ClearWindow() {
    if (fit == WindowFit::none) {
        const uint8_t panel = lcdEnabled ? 0xff : 0;
        SDL_SetRenderDrawColor(renderer, panel, panel, panel, 0xff);
        SDL_RenderClear(renderer);
        return;
    }
    SDL_SetRenderDrawColor(renderer, background >> 16, (background >> 8) & 0xff, background & 0xff, 0xff);
    SDL_RenderClear(renderer);
    const bool landscape = rotation == Rotation::landscape_90 || rotation == Rotation::landscape_270;
    const int screenHeight = displayConfiguration.height + displayConfiguration.graffitiHeight;
    const SDL_Rect screen{deviceScreenLeft(deviceControls(deviceType),controlView), 0, landscape ? screenHeight : displayConfiguration.width,
                          landscape ? displayConfiguration.width : screenHeight};
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
