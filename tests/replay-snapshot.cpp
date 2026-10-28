// Replay snapshots with the LCD off save a black frame of the normal size and are recorded
// as lcd-off. A snapshot that cannot be written is recorded as failed and the replay goes on.
#include <SDL.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "InputReplay.h"
#include "SDL_image.h"
#include "SdlRenderer.h"
#include "SoC.h"

namespace {

class FakeSoC : public SoC {
   public:
    bool lcd{true};
    uint32_t frame[320 * 320]{};
    bool framePending{false};
    unsigned keyDowns{0};

    uint64_t Run(uint64_t, uint64_t) override { return 0; }
    bool RunToPaceSyscall(uint16_t, uint64_t, uint64_t) override { return false; }
    bool ExecuteInjected(uint64_t, uint64_t) override { return false; }
    bool Save() override { return false; }
    bool Load(size_t, void*) override { return false; }
    void DumpMMU() override {}
    void JamKey(enum KeyId, uint32_t) override {}
    uint64_t GetTime() override { return 0; }
    uint32_t* GetPendingFrame(uint32_t& first, uint32_t& last) override {
        if (!framePending) return nullptr;
        framePending = false;
        first = 0;
        last = 319;
        return frame;
    }
    void ResetPendingFrame() override {}
    DeviceType5 GetDeviceType() override { return DeviceType5::deviceTypeTX; }
    DisplayMode GetDisplayMode() override { return DisplayMode::mode_320x480; }
    void SuspendTimerInterrupts(bool) override {}
    bool LcdEnabled() override { return lcd; }

   protected:
    void OnSetFramebufferDirty() override {}
    bool OnSleep() override { return false; }
    void OnWakeup() override {}
    void OnSetAudioQueue(struct AudioQueue*) override {}
    void OnSetPcmOutputEnabled() override {}
    void OnSetPcmSuspended() override {}
    void OnSdInsert() override {}
    void OnSdRemount() override {}
    void OnSdEject() override {}
};

void checkPng(const std::string& path, int width, int height, bool black) {
    SDL_Surface* loaded = IMG_Load(path.c_str());
    assert(loaded);
    SDL_Surface* surface = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    assert(surface && surface->w == width && surface->h == height);
    const auto* pixels = static_cast<const uint8_t*>(surface->pixels);
    bool allBlack = true;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = pixels + y * surface->pitch + x * 4;
            if (p[0] || p[1] || p[2] || p[3] != 0xff) allBlack = false;
        }
    assert(allBlack == black);
    SDL_FreeSurface(surface);
}

}  // namespace

int main() {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    char directory[] = "/tmp/frond-replay-snapshot-XXXXXX";
    assert(mkdtemp(directory));
    const std::string root = directory;

    DisplayConfiguration display{320, 320, 160, 2};
    SDL_Window* window = SDL_CreateWindow("test", 0, 0, 320, 480, SDL_WINDOW_HIDDEN);
    assert(window);
    FakeSoC soc;
    for (auto& pixel : soc.frame) pixel = 0xff3366ccu;
    soc.framePending = true;
    {
        SdlRenderer renderer(window, &soc, 1, display, Rotation::portrait_0);
        renderer.Draw(true);

        const std::string replayPath = root + "/snapshots.replay";
        std::ofstream(replayPath) << "0 snapshot \"" << root << "/lit.png\"\n"
                                  << "10 snapshot \"" << root << "/off.png\"\n"
                                  << "20 snapshot \"" << root << "/missing/dir.png\"\n"
                                  << "30 key-down home\n";
        InputReplay replay;
        std::string error;
        assert(replay.Load(replayPath, 320, 480, error));

        replay.Dispatch(0, soc, renderer);
        soc.lcd = false;
        renderer.Draw(false);
        replay.Dispatch(30, soc, renderer);
        assert(replay.Finished());

        const auto& results = replay.SnapshotResults();
        assert(results.size() == 3);
        assert(results[0] == "ok" && results[1] == "lcd-off" && results[2] == "failed");
        checkPng(root + "/lit.png", 320, 320, false);
        checkPng(root + "/off.png", 320, 320, true);
        assert(access((root + "/missing/dir.png").c_str(), F_OK) != 0);

        // Before any frame has been drawn the snapshot is black too, labelled no-frame.
        FakeSoC fresh;
        SDL_Window* second = SDL_CreateWindow("fresh", 0, 0, 320, 480, SDL_WINDOW_HIDDEN);
        assert(second);
        SdlRenderer unused(second, &fresh, 1, display, Rotation::portrait_0);
        InputReplay early;
        std::ofstream(replayPath) << "0 snapshot \"" << root << "/early.png\"\n";
        assert(early.Load(replayPath, 320, 480, error));
        early.Dispatch(0, fresh, unused);
        assert(early.SnapshotResults().size() == 1 && early.SnapshotResults()[0] == "no-frame");
        checkPng(root + "/early.png", 320, 320, true);
        unlink((root + "/early.png").c_str());
        unlink(replayPath.c_str());
    }
    // The second window outlives its renderer only until here; SDL frees it with SDL_Quit.
    unlink((root + "/lit.png").c_str());
    unlink((root + "/off.png").c_str());
    rmdir(directory);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("replay snapshot: ok");
    return 0;
}
