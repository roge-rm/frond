// Android entry point. SDL calls SDL_main on its own thread with the arguments from
// FrondActivity.getArguments(). This sends stdout and stderr to logcat and to files/core.log,
// moves the process into app storage and runs the desktop front end's main().
#include <SDL.h>
#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

int frond_core_main(int argc, char** argv);

#define LOG(...) logFormat(__VA_ARGS__)

namespace {
    int stdioPipe[2] = {-1, -1};
    std::thread stdioThread;

    // The core's output for this run, for the app's log screen. Started fresh on each run; past
    // LOG_LIMIT bytes it moves to core.log.1, so the two files hold at most twice that.
    constexpr long LOG_LIMIT = 256 * 1024;
    std::mutex logMutex;
    std::string logPath;
    FILE* logFile = nullptr;

    void openLog(const std::string& folder) {
        std::lock_guard<std::mutex> lock(logMutex);
        logPath = folder + "/core.log";
        std::remove((logPath + ".1").c_str());
        logFile = std::fopen(logPath.c_str(), "w");
    }

    // One line to logcat and the log file. Called from the stdio thread and SDL's thread.
    void logLine(const char* text) {
        __android_log_write(ANDROID_LOG_INFO, "Frond", text);
        std::lock_guard<std::mutex> lock(logMutex);
        if (!logFile) return;
        std::fputs(text, logFile);
        std::fputc('\n', logFile);
        std::fflush(logFile);
        if (std::ftell(logFile) < LOG_LIMIT) return;
        std::fclose(logFile);
        std::rename(logPath.c_str(), (logPath + ".1").c_str());
        logFile = std::fopen(logPath.c_str(), "w");
    }

    __attribute__((format(printf, 1, 2))) void logFormat(const char* format, ...) {
        char text[2048];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        logLine(text);
    }

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
                    logLine(line.c_str());
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
        openLog(home);
        const std::string tmp = home + "/tmp";
        mkdir(tmp.c_str(), 0700);
        if (chdir(home.c_str()) != 0) LOG("chdir %s failed", home.c_str());
        setenv("HOME", home.c_str(), 1);
        setenv("TMPDIR", tmp.c_str(), 1);
        LOG("storage: cwd and HOME %s, TMPDIR %s", home.c_str(), tmp.c_str());
    }
}  // namespace

extern "C" int SDL_main(int argc, char** argv) {
    enterAppStorage();
    redirectStdio();
    std::string line;
    for (int i = 1; i < argc; ++i) line += std::string(" ") + argv[i];
    LOG("starting core:%s", line.c_str());
    const int status = frond_core_main(argc, argv);
    std::fflush(stdout);
    LOG("core exited with %d", status);
    return status;
}
