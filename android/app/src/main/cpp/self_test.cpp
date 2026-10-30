// M1 self-test: runs the core's CPU tests from tests/ with no firmware, opens an
// SDL window to prove the Java glue works, and logs everything under the tag Frond.
#include <SDL.h>
#include <android/log.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

void frond_arm_alignment();
void frond_instruction_cache(int argc, char** argv);

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Frond", __VA_ARGS__)

// Android drops stdout and stderr, so copy each line of both to logcat.
static void redirectStdio() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    std::thread([] {
        std::string line;
        char buffer[512];
        ssize_t got;
        while ((got = read(fds[0], buffer, sizeof(buffer))) > 0) {
            for (ssize_t i = 0; i < got; ++i) {
                if (buffer[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, "Frond", line.c_str());
                    line.clear();
                } else {
                    line += buffer[i];
                }
            }
        }
    }).detach();
}

static void logEnvironment() {
    char cwd[512] = "?";
    if (!getcwd(cwd, sizeof(cwd))) std::strcpy(cwd, "(none)");
    const char* home = std::getenv("HOME");
    const char* tmp = std::getenv("TMPDIR");
    LOG("env: cwd=%s HOME=%s TMPDIR=%s /tmp %s", cwd, home ? home : "(unset)", tmp ? tmp : "(unset)",
        access("/tmp", W_OK) == 0 ? "writable" : "missing or read-only");
    LOG("env: internal=%s external=%s", SDL_AndroidGetInternalStoragePath(),
        SDL_AndroidGetExternalStoragePath() ? SDL_AndroidGetExternalStoragePath() : "(none)");
}

struct Screen {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    int w{}, h{};
};

static bool openWindow(Screen& screen) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    screen.window = SDL_CreateWindow("Frond", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0, 0,
                                     SDL_WINDOW_FULLSCREEN);
    if (screen.window) screen.renderer = SDL_CreateRenderer(screen.window, -1, SDL_RENDERER_ACCELERATED);
    if (!screen.renderer) {
        LOG("SDL window failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }
    SDL_GetRendererOutputSize(screen.renderer, &screen.w, &screen.h);
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(screen.renderer, &info);
    LOG("SDL window %dx%d, renderer %s", screen.w, screen.h, info.name);
    return true;
}

// A grey-green square on a dark background; green once the tests pass.
static void draw(Screen& screen, bool passed) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {}
    SDL_SetRenderDrawColor(screen.renderer, 0x1e, 0x2a, 0x22, 0xff);
    SDL_RenderClear(screen.renderer);
    SDL_Rect square{screen.w / 2 - 160, screen.h / 2 - 160, 320, 320};
    if (passed) SDL_SetRenderDrawColor(screen.renderer, 0x9b, 0xbc, 0x0f, 0xff);
    else SDL_SetRenderDrawColor(screen.renderer, 0x60, 0x68, 0x60, 0xff);
    SDL_RenderFillRect(screen.renderer, &square);
    SDL_RenderPresent(screen.renderer);
}

static void closeWindow(Screen& screen) {
    SDL_DestroyRenderer(screen.renderer);
    SDL_DestroyWindow(screen.window);
    SDL_Quit();
}

extern "C" int SDL_main(int, char**) {
    redirectStdio();
    SDL_version linked;
    SDL_GetVersion(&linked);
    LOG("Frond self-test, SDL %d.%d.%d, ABI %s", linked.major, linked.minor, linked.patch,
#if defined(__aarch64__)
        "arm64-v8a"
#elif defined(__x86_64__)
        "x86_64"
#else
        "other"
#endif
    );
    logEnvironment();
    Screen screen;
    const bool shown = openWindow(screen);
    if (shown) draw(screen, false);
    // A failed check aborts in the test itself; bionic logs the assertion to logcat.
    frond_arm_alignment();
    LOG("arm-alignment done");
    char name[] = "instruction-cache";
    char bench[] = "bench";
    char* argv[] = {name, bench, nullptr};
    frond_instruction_cache(2, argv);
    LOG("instruction-cache done");
    std::fflush(stdout);
    LOG("self-test passed");
    if (shown) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end) {
            draw(screen, true);
            SDL_Delay(16);
        }
        closeWindow(screen);
    }
    usleep(100000);  // let the logging thread drain
    return 0;
}
