#ifndef FROND_IMX1_GRAPHICS_GPU_GLES_H
#define FROND_IMX1_GRAPHICS_GPU_GLES_H

#include "imx1_graphics_gpu.h"

// The GLES 3 backend over EGL, for the Linux desktop and Android alike. libEGL and libGLESv2 are opened at run
// time, so nothing links against them and a host without them just has no GPU. It draws on a thread of its own
// that owns the context, so it never meets a front end's own GL context on the emulation thread.
#if __has_include(<EGL/egl.h>) && __has_include(<GLES3/gl3.h>) && !defined(__EMSCRIPTEN__)
#define FROND_IMX1_GPU_GLES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

class Imx1GraphicsGpuGles final : public Imx1GraphicsGpuBackend {
   public:
    // bankBytes: each bank's size, a whole number of pages. Null with why in error when there is no usable GPU.
    // FROND_ZODIAC_GPU_DEVICE names an EGL device's DRM node to use (e.g. /dev/dri/renderD128), or surfaceless;
    // otherwise the first hardware device with a DRM node, then Mesa's surfaceless platform, then the default
    // display (Android's).
    static std::unique_ptr<Imx1GraphicsGpuGles> create(const uint32_t (&bankBytes)[2], std::string& error) {
        std::unique_ptr<Imx1GraphicsGpuGles> gpu(new Imx1GraphicsGpuGles());
        for (unsigned bank = 0; bank < 2; ++bank) gpu->rows[bank] = bankBytes[bank] / pageBytes;
        gpu->worker = std::thread([raw = gpu.get()] { raw->run(); });
        {
            std::unique_lock<std::mutex> lock(gpu->mutex);
            gpu->changed.wait(lock, [&] { return gpu->started; });
        }
        if (!gpu->ready) {
            error = gpu->failure;
            return nullptr;
        }
        return gpu;
    }
    ~Imx1GraphicsGpuGles() override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
        if (posted && !std::getenv("FROND_ZODIAC_GPU_QUIET"))
            std::fprintf(stderr, "zodiac gpu: per command %.1f us queued, %.1f us drawing; per batch %.1f us reading back\n",
                         stats.queueNs / 1e3 / double(posted), stats.drawNs / 1e3 / double(posted),
                         stats.batches ? stats.readNs / 1e3 / double(stats.batches) : 0.0);
    }
    const std::string& renderer() const override { return name; }
    bool healthy() const override { return ready && !broken; }
    void setReplica(uint8_t* bank0, uint8_t* bank1) override {
        drain();
        replica[0] = bank0;
        replica[1] = bank1;
    }
    uint64_t upload(unsigned bank, uint32_t firstPage, uint32_t pages, Bytes data) override {
        Job job{Job::upload, 0, bank, firstPage, pages, std::move(data), {}, nullptr, {}};
        return post(std::move(job));
    }
    uint64_t fill(const Imx1GpuFill& fill, Pixels pixels, uint32_t firstPage, uint32_t pages) override {
        Job job{Job::fill, 0, fill.bank, firstPage, pages, nullptr, fill, std::move(pixels), {}};
        return post(std::move(job));
    }
    uint64_t patch(unsigned bank, uint32_t firstPage, uint32_t pages, Bytes data) override {
        Job job{Job::patch, 0, bank, firstPage, pages, std::move(data), {}, nullptr, {}};
        return post(std::move(job));
    }
    uint64_t confirmed() const override { return done.load(std::memory_order_acquire); }
    bool waitConfirmed(uint64_t number) override {
        // Batches are short: spin a little before sleeping.
        for (unsigned i = 0; i < 2000; ++i) {
            if (confirmed() >= number) return true;
            if (broken) break;
        }
        std::unique_lock<std::mutex> lock(mutex);
        finished.wait(lock, [&] { return confirmed() >= number || broken; });
        return confirmed() >= number;
    }
    void drain() override {
        std::unique_lock<std::mutex> lock(mutex);
        finished.wait(lock, [&] { return taken >= posted; });
    }
    const Stats& backendStatistics() const override { return stats; }
    void injectError(uint64_t number) override { injectAt = number; }
    void hold(bool held) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            holding = held;
        }
        changed.notify_all();
    }

   private:
    Imx1GraphicsGpuGles() = default;
    struct Functions {
        PFNGLGETSTRINGPROC GetString;
        PFNGLGETERRORPROC GetError;
        PFNGLGETINTEGERVPROC GetIntegerv;
        PFNGLGENTEXTURESPROC GenTextures;
        PFNGLBINDTEXTUREPROC BindTexture;
        PFNGLTEXPARAMETERIPROC TexParameteri;
        PFNGLTEXSTORAGE2DPROC TexStorage2D;
        PFNGLTEXIMAGE2DPROC TexImage2D;
        PFNGLTEXSUBIMAGE2DPROC TexSubImage2D;
        PFNGLPIXELSTOREIPROC PixelStorei;
        PFNGLACTIVETEXTUREPROC ActiveTexture;
        PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
        PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
        PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
        PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
        PFNGLREADPIXELSPROC ReadPixels;
        PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
        PFNGLVIEWPORTPROC Viewport;
        PFNGLSCISSORPROC Scissor;
        PFNGLENABLEPROC Enable;
        PFNGLDISABLEPROC Disable;
        PFNGLCREATESHADERPROC CreateShader;
        PFNGLSHADERSOURCEPROC ShaderSource;
        PFNGLCOMPILESHADERPROC CompileShader;
        PFNGLGETSHADERIVPROC GetShaderiv;
        PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
        PFNGLCREATEPROGRAMPROC CreateProgram;
        PFNGLATTACHSHADERPROC AttachShader;
        PFNGLLINKPROGRAMPROC LinkProgram;
        PFNGLGETPROGRAMIVPROC GetProgramiv;
        PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
        PFNGLUSEPROGRAMPROC UseProgram;
        PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
        PFNGLUNIFORM1UIPROC Uniform1ui;
        PFNGLUNIFORM4UIVPROC Uniform4uiv;
        PFNGLUNIFORM1IPROC Uniform1i;
        PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
        PFNGLBINDVERTEXARRAYPROC BindVertexArray;
        PFNGLDRAWARRAYSPROC DrawArrays;
        PFNGLGENBUFFERSPROC GenBuffers;
        PFNGLBINDBUFFERPROC BindBuffer;
        PFNGLBUFFERDATAPROC BufferData;
        PFNGLMAPBUFFERRANGEPROC MapBufferRange;
        PFNGLUNMAPBUFFERPROC UnmapBuffer;
        PFNGLFENCESYNCPROC FenceSync;
        PFNGLCLIENTWAITSYNCPROC ClientWaitSync;
        PFNGLDELETESYNCPROC DeleteSync;
    };
    struct Egl {
        PFNEGLGETPROCADDRESSPROC GetProcAddress;
        PFNEGLQUERYSTRINGPROC QueryString;
        PFNEGLGETDISPLAYPROC GetDisplay;
        PFNEGLINITIALIZEPROC Initialize;
        PFNEGLTERMINATEPROC Terminate;
        PFNEGLBINDAPIPROC BindAPI;
        PFNEGLCHOOSECONFIGPROC ChooseConfig;
        PFNEGLCREATECONTEXTPROC CreateContext;
        PFNEGLDESTROYCONTEXTPROC DestroyContext;
        PFNEGLCREATEPBUFFERSURFACEPROC CreatePbufferSurface;
        PFNEGLDESTROYSURFACEPROC DestroySurface;
        PFNEGLMAKECURRENTPROC MakeCurrent;
        PFNEGLGETERRORPROC GetError;
    };
    struct Bank { GLuint texture = 0, framebuffer = 0; };
    enum Unit { sourceUnit, underUnit, pixelsUnit };

    struct Job {
        enum Kind { upload, fill, patch } kind;
        uint64_t number;
        unsigned bank;
        uint32_t firstPage, pages;
        Bytes data;
        Imx1GpuFill drawn;
        Pixels pixels;
        std::chrono::steady_clock::time_point queued;
    };
    uint64_t post(Job job) {
        job.number = ++posted;
        job.queued = std::chrono::steady_clock::now();
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            jobs.push_back(std::move(job));
            wake = idle;
        }
        // Only a sleeping worker needs waking; a busy one takes the job with its next batch.
        if (wake) changed.notify_all();
        return posted;
    }
    // Takes every command posted so far as one batch, runs them, then reads back the pages they changed and
    // confirms the batch. After an error it only takes commands, so a waiter sees broken.
    void run() {
        const bool ok = start();
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = ok;
            started = true;
        }
        changed.notify_all();
        if (ok) {
            for (;;) {
                std::deque<Job> batch;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    idle = true;
                    changed.wait(lock, [&] { return stopping || (!jobs.empty() && !holding); });
                    idle = false;
                    if (jobs.empty()) break;
                    batch.swap(jobs);
                }
                const auto started = std::chrono::steady_clock::now();
                using ns = std::chrono::nanoseconds;
                for (auto& job : batch) {
                    stats.queueNs += uint64_t(std::chrono::duration_cast<ns>(started - job.queued).count());
                    if (!broken) execute(job);
                }
                const auto drawn = std::chrono::steady_clock::now();
                stats.drawNs += uint64_t(std::chrono::duration_cast<ns>(drawn - started).count());
                if (!broken) finish(batch.back().number);
                stats.readNs += uint64_t(std::chrono::duration_cast<ns>(std::chrono::steady_clock::now() - drawn).count());
                ++stats.batches;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    taken = batch.back().number;
                }
                finished.notify_all();
            }
        }
        stop();
    }
    void execute(const Job& job) {
        if (job.number == injectAt) gl.Enable(0);  // GL_INVALID_ENUM, for tests
        if (job.kind == Job::upload) {
            gl.BindTexture(GL_TEXTURE_2D, banks[job.bank].texture);
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(job.firstPage), rowTexels, GLsizei(job.pages), GL_RED_INTEGER,
                             GL_UNSIGNED_SHORT, job.data->data());
            for (uint32_t i = 0; i < job.pages; ++i)
                uploaded.push_back({job.bank, job.firstPage + i, job.data, size_t(i) * pageBytes});
            return;
        }
        if (job.kind == Job::fill) draw(job.drawn, job.pixels ? job.pixels->data() : nullptr);
        else drawPatch(job);
        auto& rowsChanged = changedRows[job.bank];
        if (rowsChanged.size() != rows[job.bank]) rowsChanged.assign(rows[job.bank], 0);
        for (uint32_t i = 0; i < job.pages; ++i) rowsChanged[job.firstPage + i] = 1;
    }
    // Reads the batch's changed rows into a pixel buffer, waits for its fence, and only once no GL error has shown
    // copies them and the batch's uploads into the replica and confirms last. On an error nothing of the batch
    // reaches the replica, so it still holds the GPU copy as of the last confirm.
    void finish(uint64_t last) {
        const size_t texelBytes = readShort ? 2 : 16;
        struct Run { unsigned bank; uint32_t first, count; size_t at; };
        std::vector<Run> runs;
        size_t total = 0;
        for (unsigned bank = 0; bank < 2; ++bank) {
            auto& rowsChanged = changedRows[bank];
            for (uint32_t row = 0; row < rowsChanged.size();) {
                if (!rowsChanged[row]) { ++row; continue; }
                uint32_t end = row;
                while (end < rowsChanged.size() && rowsChanged[end]) rowsChanged[end++] = 0;
                runs.push_back({bank, row, end - row, total});
                total += size_t(end - row) * rowTexels * texelBytes;
                row = end;
            }
        }
        if (!runs.empty()) {
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
            if (total > packBytes) {
                packBytes = total;
                gl.BufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(packBytes), nullptr, GL_STREAM_READ);
            }
            gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
            for (const auto& run : runs) {
                gl.BindFramebuffer(GL_READ_FRAMEBUFFER, banks[run.bank].framebuffer);
                gl.ReadPixels(0, GLint(run.first), rowTexels, GLsizei(run.count), readShort ? GL_RED_INTEGER : GL_RGBA_INTEGER,
                              readShort ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, reinterpret_cast<void*>(run.at));
            }
            GLsync fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            // A fence that doesn't signal in two seconds counts as a lost device.
            GLenum waited = GL_TIMEOUT_EXPIRED;
            const auto waitStart = std::chrono::steady_clock::now();
            while (fence && waited == GL_TIMEOUT_EXPIRED) {
                waited = gl.ClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000);
                if (waited == GL_TIMEOUT_EXPIRED && std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(2)) break;
            }
            if (fence) gl.DeleteSync(fence);
            if (waited == GL_TIMEOUT_EXPIRED || waited == GL_WAIT_FAILED || !fence) {
                if (waited == GL_TIMEOUT_EXPIRED) std::fprintf(stderr, "zodiac gpu: fence timed out, drawing goes back to the CPU\n");
                check("fence");
                broken = true;
            }
            if (broken || !check("batch")) {
                gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                uploaded.clear();
                return;
            }
            const void* mapped = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(total), GL_MAP_READ_BIT);
            staging.resize(total / texelBytes * 2);
            if (mapped) {
                if (readShort) std::memcpy(staging.data(), mapped, total);
                else {
                    // GLES reads an unsigned integer buffer as RGBA unsigned ints.
                    const uint32_t* texels = static_cast<const uint32_t*>(mapped);
                    for (size_t i = 0; i < total / texelBytes; ++i) {
                        staging[i * 2] = uint8_t(texels[i * 4]);
                        staging[i * 2 + 1] = uint8_t(texels[i * 4] >> 8);
                    }
                }
            }
            const bool unmapped = mapped && gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            if (!mapped || !unmapped) broken = true;
            if (broken || !check("readback")) {
                uploaded.clear();
                finished.notify_all();
                return;
            }
            ++stats.readbacks;
            stats.readbackBytes += total;
        } else if (!check("batch")) {
            uploaded.clear();
            return;
        }
        // Uploads to pages no command of the batch changed; the changed ones come from the readback.
        for (const auto& upload : uploaded) {
            bool read = false;
            for (const auto& run : runs)
                read = read || (run.bank == upload.bank && upload.page >= run.first && upload.page < run.first + run.count);
            if (!read) std::memcpy(replica[upload.bank] + size_t(upload.page) * pageBytes, upload.data->data() + upload.at, pageBytes);
        }
        uploaded.clear();
        for (const auto& run : runs)
            std::memcpy(replica[run.bank] + size_t(run.first) * pageBytes, staging.data() + run.at / texelBytes * 2,
                        size_t(run.count) * pageBytes);
        {
            std::lock_guard<std::mutex> lock(mutex);
            done.store(last, std::memory_order_release);
        }
        finished.notify_all();
    }
    template <class F> bool load(void* library, F& to, const char* symbol) {
        void* found = library ? dlsym(library, symbol) : nullptr;
        if (!found && egl.GetProcAddress) found = reinterpret_cast<void*>(egl.GetProcAddress(symbol));
        to = reinterpret_cast<F>(found);
        if (!found) failure = std::string("no ") + symbol;
        return found;
    }
    static void* open(std::initializer_list<const char*> names) {
        for (const char* n : names)
            if (void* library = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return library;
        return nullptr;
    }
    static bool hasWord(const char* list, const char* word) {
        if (!list) return false;
        const size_t length = std::strlen(word);
        for (const char* at = std::strstr(list, word); at; at = std::strstr(at + 1, word))
            if ((at == list || at[-1] == ' ') && (at[length] == ' ' || !at[length])) return true;
        return false;
    }
    EGLDisplay pickDisplay() {
        const char* client = egl.QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            egl.GetProcAddress("eglGetPlatformDisplayEXT"));
        auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(egl.GetProcAddress("eglQueryDevicesEXT"));
        auto deviceString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(egl.GetProcAddress("eglQueryDeviceStringEXT"));
        const char* wanted = std::getenv("FROND_ZODIAC_GPU_DEVICE");
        // "surfaceless" skips the devices, for Mesa's software rasteriser.
        const bool devices = !wanted || std::strcmp(wanted, "surfaceless");
        if (!devices) wanted = nullptr;
        if (devices && getPlatformDisplay && queryDevices && deviceString && hasWord(client, "EGL_EXT_platform_device")) {
            EGLDeviceEXT devices[16];
            EGLint count = 0;
            if (queryDevices(16, devices, &count)) {
                for (EGLint i = 0; i < count; ++i) {
                    const char* extensions = deviceString(devices[i], EGL_EXTENSIONS);
                    if (hasWord(extensions, "EGL_MESA_device_software")) continue;
                    const char* node = hasWord(extensions, "EGL_EXT_device_drm_render_node") ?
                        deviceString(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT) : nullptr;
                    const char* card = hasWord(extensions, "EGL_EXT_device_drm") ?
                        deviceString(devices[i], EGL_DRM_DEVICE_FILE_EXT) : nullptr;
                    if (wanted && !((node && !std::strcmp(node, wanted)) || (card && !std::strcmp(card, wanted)))) continue;
                    if (!wanted && !node && !card) continue;
                    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
                    if (display != EGL_NO_DISPLAY && egl.Initialize(display, nullptr, nullptr)) {
                        device = node ? node : card ? card : "";
                        return display;
                    }
                }
            }
        }
        if (wanted) {
            failure = std::string("no EGL device ") + wanted;
            return EGL_NO_DISPLAY;
        }
        if (getPlatformDisplay && hasWord(client, "EGL_MESA_platform_surfaceless")) {
            EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            if (display != EGL_NO_DISPLAY && egl.Initialize(display, nullptr, nullptr)) {
                device = "surfaceless";
                return display;
            }
        }
        EGLDisplay display = egl.GetDisplay(EGL_DEFAULT_DISPLAY);
        if (display != EGL_NO_DISPLAY && egl.Initialize(display, nullptr, nullptr)) {
            device = "default";
            return display;
        }
        failure = "no EGL display";
        return EGL_NO_DISPLAY;
    }
    bool start() {
        eglLibrary = open({"libEGL.so.1", "libEGL.so"});
        glLibrary = open({"libGLESv2.so.2", "libGLESv3.so", "libGLESv2.so"});
        if (!eglLibrary || !glLibrary) {
            failure = "no libEGL or libGLESv2";
            return false;
        }
        egl.GetProcAddress = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(dlsym(eglLibrary, "eglGetProcAddress"));
        if (!egl.GetProcAddress) { failure = "no eglGetProcAddress"; return false; }
        if (!load(eglLibrary, egl.QueryString, "eglQueryString") || !load(eglLibrary, egl.GetDisplay, "eglGetDisplay") ||
            !load(eglLibrary, egl.Initialize, "eglInitialize") || !load(eglLibrary, egl.Terminate, "eglTerminate") ||
            !load(eglLibrary, egl.BindAPI, "eglBindAPI") || !load(eglLibrary, egl.ChooseConfig, "eglChooseConfig") ||
            !load(eglLibrary, egl.CreateContext, "eglCreateContext") ||
            !load(eglLibrary, egl.DestroyContext, "eglDestroyContext") ||
            !load(eglLibrary, egl.CreatePbufferSurface, "eglCreatePbufferSurface") ||
            !load(eglLibrary, egl.DestroySurface, "eglDestroySurface") ||
            !load(eglLibrary, egl.MakeCurrent, "eglMakeCurrent") || !load(eglLibrary, egl.GetError, "eglGetError"))
            return false;
        display = pickDisplay();
        if (display == EGL_NO_DISPLAY) return false;
        if (!egl.BindAPI(EGL_OPENGL_ES_API)) { failure = "no GLES API"; return false; }
        const bool surfaceless = hasWord(egl.QueryString(display, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context");
        const EGLint pbufferConfig[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        const EGLint anyConfig[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        bool pbuffer = egl.ChooseConfig(display, pbufferConfig, &config, 1, &configs) && configs;
        if (!pbuffer && !(surfaceless && egl.ChooseConfig(display, anyConfig, &config, 1, &configs) && configs)) {
            failure = "no GLES 3 config";
            return false;
        }
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = egl.CreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        if (context == EGL_NO_CONTEXT) { failure = "no GLES 3 context"; return false; }
        if (!surfaceless) {
            const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            surface = egl.CreatePbufferSurface(display, config, size);
            if (surface == EGL_NO_SURFACE) { failure = "no pbuffer"; return false; }
        }
        if (!egl.MakeCurrent(display, surface, surface, context)) { failure = "eglMakeCurrent failed"; return false; }
#define FROND_GL(f) load(glLibrary, gl.f, "gl" #f)
        if (!(FROND_GL(GetString) && FROND_GL(GetError) && FROND_GL(GetIntegerv) && FROND_GL(GenTextures) &&
              FROND_GL(BindTexture) && FROND_GL(TexParameteri) && FROND_GL(TexStorage2D) && FROND_GL(TexImage2D) &&
              FROND_GL(TexSubImage2D) && FROND_GL(PixelStorei) && FROND_GL(ActiveTexture) &&
              FROND_GL(GenFramebuffers) && FROND_GL(BindFramebuffer) && FROND_GL(FramebufferTexture2D) &&
              FROND_GL(CheckFramebufferStatus) && FROND_GL(ReadPixels) && FROND_GL(BlitFramebuffer) &&
              FROND_GL(Viewport) && FROND_GL(Scissor) && FROND_GL(Enable) && FROND_GL(Disable) &&
              FROND_GL(CreateShader) && FROND_GL(ShaderSource) && FROND_GL(CompileShader) && FROND_GL(GetShaderiv) &&
              FROND_GL(GetShaderInfoLog) && FROND_GL(CreateProgram) && FROND_GL(AttachShader) &&
              FROND_GL(LinkProgram) && FROND_GL(GetProgramiv) && FROND_GL(GetProgramInfoLog) &&
              FROND_GL(UseProgram) && FROND_GL(GetUniformLocation) && FROND_GL(Uniform1ui) && FROND_GL(Uniform4uiv) && FROND_GL(Uniform1i) &&
              FROND_GL(GenVertexArrays) && FROND_GL(BindVertexArray) && FROND_GL(DrawArrays) &&
              FROND_GL(GenBuffers) && FROND_GL(BindBuffer) && FROND_GL(BufferData) && FROND_GL(MapBufferRange) &&
              FROND_GL(UnmapBuffer) && FROND_GL(FenceSync) && FROND_GL(ClientWaitSync) && FROND_GL(DeleteSync)))
            return false;
#undef FROND_GL
        const char* renderer = reinterpret_cast<const char*>(gl.GetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(gl.GetString(GL_VERSION));
        name = std::string(renderer ? renderer : "?") + " (" + (version ? version : "?") + ", " + device + ")";
        for (unsigned bank = 0; bank < 2; ++bank) {
            banks[bank] = makeTarget(rows[bank]);
            if (!banks[bank].texture) return false;
        }
        const uint32_t scratchRows = std::max(rows[0], rows[1]);
        sourceCopy = makeTarget(scratchRows);
        underCopy = makeTarget(scratchRows);
        if (!sourceCopy.texture || !underCopy.texture) return false;
        gl.GenTextures(1, &pixelTexture);
        gl.BindTexture(GL_TEXTURE_2D, pixelTexture);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.GenTextures(1, &patchTexture);
        gl.BindTexture(GL_TEXTURE_2D, patchTexture);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.GenBuffers(1, &packBuffer);
        if (!makePatchProgram() || !makeProgram()) return false;
        gl.GenVertexArrays(1, &vertexArray);
        gl.BindVertexArray(vertexArray);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, banks[1].framebuffer);
        GLint format = 0, type = 0;
        gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
        gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
        readShort = format == GL_RED_INTEGER && type == GL_UNSIGNED_SHORT;
        if (std::getenv("FROND_ZODIAC_GPU_READ_RGBA")) readShort = false;
        return check("start");
    }
    void stop() {
        if (display != EGL_NO_DISPLAY) {
            egl.MakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) egl.DestroySurface(display, surface);
            if (context != EGL_NO_CONTEXT) egl.DestroyContext(display, context);
            egl.Terminate(display);
        }
        // The libraries stay loaded: some drivers keep threads that outlive the display.
    }
    bool check(const char* what) {
        const GLenum error = gl.GetError();
        if (error == GL_NO_ERROR) return true;
        if (!broken) std::fprintf(stderr, "zodiac gpu: GL error %#x in %s, drawing goes back to the CPU\n", error, what);
        failure = std::string("GL error in ") + what;
        broken = true;
        return false;
    }
    Bank makeTarget(uint32_t height) {
        Bank target;
        gl.GenTextures(1, &target.texture);
        gl.BindTexture(GL_TEXTURE_2D, target.texture);
        gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_R16UI, rowTexels, GLsizei(height));
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.GenFramebuffers(1, &target.framebuffer);
        gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            failure = "R16UI target incomplete";
            return {};
        }
        return target;
    }
    GLuint compile(GLenum kind, const char* source) {
        const GLuint shader = gl.CreateShader(kind);
        gl.ShaderSource(shader, 1, &source, nullptr);
        gl.CompileShader(shader);
        GLint ok = 0;
        gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            gl.GetShaderInfoLog(shader, sizeof log, nullptr, log);
            failure = std::string("shader: ") + log;
            return 0;
        }
        return shader;
    }
    GLuint link(const char* vertexSource, const char* fragmentSource) {
        const GLuint v = compile(GL_VERTEX_SHADER, vertexSource), f = v ? compile(GL_FRAGMENT_SHADER, fragmentSource) : 0;
        if (!f) return 0;
        const GLuint made = gl.CreateProgram();
        gl.AttachShader(made, v);
        gl.AttachShader(made, f);
        gl.LinkProgram(made);
        GLint ok = 0;
        gl.GetProgramiv(made, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            gl.GetProgramInfoLog(made, sizeof log, nullptr, log);
            failure = std::string("link: ") + log;
            return 0;
        }
        return made;
    }
    // A patch: each texel of the patch texture holds a value and a mask, for the page row firstRow up from it.
    bool makePatchProgram() {
        static const char* fragment = R"(#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform usampler2D under;
uniform usampler2D patchTexels;
uniform uint firstRow;
layout(location = 0) out uint result;
void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    uvec2 value = texelFetch(patchTexels, ivec2(at.x, at.y - int(firstRow)), 0).rg;
    if (value.g == 0u) discard;
    uint d = texelFetch(under, at, 0).r;
    result = ((d & ~value.g) | (value.r & value.g)) & 65535u;
})";
        patchProgram = link(vertexSource(), fragment);
        if (!patchProgram) return false;
        gl.UseProgram(patchProgram);
        gl.Uniform1i(gl.GetUniformLocation(patchProgram, "under"), underUnit);
        gl.Uniform1i(gl.GetUniformLocation(patchProgram, "patchTexels"), pixelsUnit);
        patchFirst = gl.GetUniformLocation(patchProgram, "firstRow");
        return true;
    }
    // A triangle over the whole target; the scissor picks the rows.
    static const char* vertexSource() {
        return R"(#version 300 es
void main() {
    vec2 at = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));
    gl_Position = vec4(at, 0.0, 1.0);
})";
    }
    bool makeProgram() {
        // One texel per destination address: which pixel of the fill it is, if any, and that pixel's colour as the
        // CPU loop works it out (inline pixels, source copy with key and paint, invert, A4).
        static const char* fragment = R"(#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform usampler2D source;
uniform usampler2D under;
uniform usampler2D pixels;
// The fill's values in one array, set with one call: base, pitch, width, height, colour, sourceBase, sourcePitch,
// sourceX, sourceY, paint, flags.
uniform uvec4 values[3];
layout(location = 0) out uint result;
uint at(usampler2D from, uint address) {
    return texelFetch(from, ivec2(int(address & 2047u), int(address >> 11)), 0).r;
}
uint blend(uint d, uint s, uint a, uint shift, uint mask) {
    return ((((d >> shift) & mask) * (15u - a) + ((s >> shift) & mask) * a) / 15u) << shift;
}
void main() {
    uint base = values[0].x, pitch = values[0].y, width = values[0].z, height = values[0].w;
    uint colour = values[1].x, sourceBase = values[1].y, sourcePitch = values[1].z, sourceX = values[1].w;
    uint sourceY = values[2].x, paint = values[2].y, flags = values[2].z;
    uint address = uint(gl_FragCoord.y) * 2048u + uint(gl_FragCoord.x);
    if (address < base) discard;
    uint offset = address - base;
    uint y = height == 1u ? 0u : offset / pitch;
    uint x = offset - y * pitch;
    if (y >= height || x >= width) discard;
    uint c = colour;
    if ((flags & 1u) != 0u) c = texelFetch(pixels, ivec2(int(x), int(y)), 0).r;
    if ((flags & 2u) != 0u) {
        uint from = (flags & 8u) != 0u ? sourceBase + ((sourceY + y) >> 1) * sourcePitch + ((sourceX + x) >> 1) :
                                         sourceBase + y * sourcePitch + x;
        c = at(source, from);
        if ((flags & 4u) != 0u && ((c == colour) != ((flags & 64u) != 0u))) discard;
        if ((flags & 128u) != 0u) c = paint;
    }
    if ((flags & 16u) != 0u) c = (at(under, address) ^ c) & 65535u;
    if ((flags & 32u) != 0u) {
        uint d = at(under, address), s = at(source, sourceBase + y * sourcePitch + x);
        c = c == 0u ? d : c == 15u ? s : blend(d, s, c, 0u, 31u) | blend(d, s, c, 5u, 63u) | blend(d, s, c, 11u, 31u);
    }
    result = c & 65535u;
})";
        program = link(vertexSource(), fragment);
        if (!program) return false;
        gl.UseProgram(program);
        gl.Uniform1i(gl.GetUniformLocation(program, "source"), sourceUnit);
        gl.Uniform1i(gl.GetUniformLocation(program, "under"), underUnit);
        gl.Uniform1i(gl.GetUniformLocation(program, "pixels"), pixelsUnit);
        uniforms = gl.GetUniformLocation(program, "values");
        return true;
    }
    // Copies rows first to last of from's bank into the same rows of a scratch target.
    void copyRows(const Bank& from, const Bank& to, uint32_t first, uint32_t last) {
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, from.framebuffer);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, to.framebuffer);
        gl.BlitFramebuffer(0, GLint(first), rowTexels, GLint(last + 1), 0, GLint(first), rowTexels, GLint(last + 1),
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    void bindUnit(Unit unit, GLuint texture) {
        gl.ActiveTexture(GL_TEXTURE0 + unit);
        gl.BindTexture(GL_TEXTURE_2D, texture);
    }
    // Writes a patch's masked bytes over its pages: the pages' own texels come from a copy of their rows.
    void drawPatch(const Job& job) {
        const size_t span = size_t(job.pages) * pageBytes;
        const uint8_t* values = job.data->data();
        const uint8_t* mask = values + span;
        patchScratch.resize(span);
        for (size_t i = 0; i < span / 2; ++i) {
            patchScratch[i * 2] = uint16_t(values[i * 2] | values[i * 2 + 1] << 8);
            patchScratch[i * 2 + 1] = uint16_t((mask[i * 2] ? 0xff : 0) | (mask[i * 2 + 1] ? 0xff00 : 0));
        }
        const Bank& target = banks[job.bank];
        const uint32_t last = job.firstPage + job.pages - 1;
        copyRows(target, underCopy, job.firstPage, last);
        bindUnit(underUnit, underCopy.texture);
        bindUnit(pixelsUnit, patchTexture);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RG16UI, rowTexels, GLsizei(job.pages), 0, GL_RG_INTEGER, GL_UNSIGNED_SHORT,
                      patchScratch.data());
        gl.UseProgram(patchProgram);
        gl.Uniform1ui(patchFirst, job.firstPage);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, target.framebuffer);
        gl.Viewport(0, 0, rowTexels, GLsizei(rows[job.bank]));
        gl.Enable(GL_SCISSOR_TEST);
        gl.Scissor(0, GLint(job.firstPage), rowTexels, GLsizei(job.pages));
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
        gl.Disable(GL_SCISSOR_TEST);
        gl.UseProgram(program);
        if (checkEach) check("patch");
    }
    void draw(const Imx1GpuFill& fill, const uint16_t* pixels) {
        if (broken) return;
        // In texels from here on.
        const uint32_t base = fill.base / 2, last = (fill.end() - 2) / 2;
        const uint32_t firstRow = base / rowTexels, lastRow = last / rowTexels;
        const Bank& target = banks[fill.bank];
        // A sampled texture is never the one drawn into: a source in the same bank and the destination's own
        // pixels are read from copies of their rows.
        GLuint source = sourceCopy.texture, under = underCopy.texture;
        if (fill.copy || fill.a4) {
            const uint32_t sourceFirst = fill.sourceBase / 2 / rowTexels;
            const uint32_t sourceLast = (fill.sourceBase + fill.sourceLength - 2) / 2 / rowTexels;
            if (fill.sourceBank == fill.bank) copyRows(target, sourceCopy, sourceFirst, sourceLast);
            else source = banks[fill.sourceBank].texture;
        }
        if (fill.invert || fill.a4) copyRows(target, underCopy, firstRow, lastRow);
        bindUnit(sourceUnit, source);
        bindUnit(underUnit, under);
        if (pixels) {
            bindUnit(pixelsUnit, pixelTexture);
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, GLsizei(fill.width), GLsizei(fill.height), 0, GL_RED_INTEGER,
                          GL_UNSIGNED_SHORT, pixels);
        } else bindUnit(pixelsUnit, pixelTexture);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, target.framebuffer);
        gl.Viewport(0, 0, rowTexels, GLsizei(rows[fill.bank]));
        gl.Enable(GL_SCISSOR_TEST);
        if (firstRow == lastRow)
            gl.Scissor(GLint(base % rowTexels), GLint(firstRow), GLsizei(last - base + 1), 1);
        else gl.Scissor(0, GLint(firstRow), rowTexels, GLsizei(lastRow - firstRow + 1));
        const uint32_t flags = (pixels ? 1u : 0) | (fill.copy ? 2u : 0) | (fill.keyed ? 4u : 0) |
            (fill.doubled ? 8u : 0) | (fill.invert ? 16u : 0) | (fill.a4 ? 32u : 0) | (fill.keyEqual ? 64u : 0) |
            (fill.painted ? 128u : 0);
        const uint32_t values[] = {base, fill.pitch / 2, fill.width, fill.height, fill.colour, fill.sourceBase / 2,
                                   fill.sourcePitch / 2, fill.sourceX, fill.sourceY, fill.paint, flags, 0};
        gl.Uniform4uiv(uniforms, 3, values);
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
        gl.Disable(GL_SCISSOR_TEST);
        if (checkEach) check("fill");
    }

    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed, finished;
    std::deque<Job> jobs;
    bool started = false, ready = false, stopping = false, holding = false, idle = false;
    // broken is set on the worker and read anywhere; ready is set once before create returns.
    std::atomic<bool> broken{false};
    // Commands posted (emulation thread), taken by the worker (under mutex) and confirmed (worker).
    uint64_t posted = 0, taken = 0;
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> injectAt{0};
    // Worker thread from here on, read by others only after it stops (stats) or once drained (replica).
    Stats stats;
    uint8_t* replica[2] = {};
    struct Uploaded { unsigned bank; uint32_t page; Bytes data; size_t at; };
    std::vector<Uploaded> uploaded;
    std::vector<uint8_t> changedRows[2];
    std::vector<uint8_t> staging;
    std::vector<uint16_t> patchScratch;
    GLuint packBuffer = 0, patchTexture = 0, patchProgram = 0;
    GLint patchFirst = -1;
    size_t packBytes = 0;
    bool readShort = false;
    const bool checkEach = std::getenv("FROND_ZODIAC_GPU_CHECK") != nullptr;
    std::string failure, name, device;
    void *eglLibrary = nullptr, *glLibrary = nullptr;
    Egl egl{};
    Functions gl{};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    uint32_t rows[2] = {};
    Bank banks[2], sourceCopy, underCopy;
    GLuint pixelTexture = 0, program = 0, vertexArray = 0;
    GLint uniforms = -1;
};

// The host's GPU backend, or null with why in error.
inline std::unique_ptr<Imx1GraphicsGpuBackend> imx1MakeGraphicsGpuBackend(const uint32_t (&bankBytes)[2],
                                                                          std::string& error) {
    return Imx1GraphicsGpuGles::create(bankBytes, error);
}
#else
inline std::unique_ptr<Imx1GraphicsGpuBackend> imx1MakeGraphicsGpuBackend(const uint32_t (&)[2], std::string& error) {
    error = "built without EGL and GLES 3";
    return nullptr;
}
#endif

#endif
