// Android entry point. SDL calls SDL_main on its own thread with the arguments from
// FrondActivity.getArguments(). This sends stdout and stderr to logcat, moves the
// process into app storage and runs the desktop front end's main().
#include <SDL.h>
#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int frond_core_main(int argc, char** argv);

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Frond", __VA_ARGS__)

namespace {
    int stdioPipe[2] = {-1, -1};
    std::thread stdioThread;

    // Android drops stdout and stderr, so copy each line of both to logcat.
    void redirectStdio() {
        if (stdioPipe[0] >= 0 || pipe(stdioPipe) != 0) return;
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(stdioPipe[1], STDOUT_FILENO);
        dup2(stdioPipe[1], STDERR_FILENO);
        stdioThread = std::thread([] {
            std::string line;
            char buffer[512];
            ssize_t got;
            while ((got = read(stdioPipe[0], buffer, sizeof(buffer))) > 0) {
                for (ssize_t i = 0; i < got; ++i) {
                    if (buffer[i] != '\n') {
                        line += buffer[i];
                        continue;
                    }
                    __android_log_write(ANDROID_LOG_INFO, "Frond", line.c_str());
                    line.clear();
                }
            }
        });
        stdioThread.detach();
    }

    // The core has no notion of app storage: give it a cwd, HOME and TMPDIR there.
    void enterAppStorage() {
        const char* files = SDL_AndroidGetInternalStoragePath();
        if (!files) {
            LOG("no internal storage path: %s", SDL_GetError());
            return;
        }
        const std::string home = files;
        const std::string tmp = home + "/tmp";
        mkdir(tmp.c_str(), 0700);
        if (chdir(home.c_str()) != 0) LOG("chdir %s failed", home.c_str());
        setenv("HOME", home.c_str(), 1);
        setenv("TMPDIR", tmp.c_str(), 1);
        LOG("storage: cwd and HOME %s, TMPDIR %s", home.c_str(), tmp.c_str());
    }
}  // namespace

extern "C" int SDL_main(int argc, char** argv) {
    redirectStdio();
    enterAppStorage();
    std::string line;
    for (int i = 1; i < argc; ++i) line += std::string(" ") + argv[i];
    LOG("starting core:%s", line.c_str());
    const int status = frond_core_main(argc, argv);
    std::fflush(stdout);
    LOG("core exited with %d", status);
    return status;
}
