#ifndef FROND_HOST_CAMERA_H
#define FROND_HOST_CAMERA_H
#include <SDL.h>
#include <SDL_image.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#define FROND_HOST_V4L2 1
#endif
#include "camera_input.h"

// Host pictures for the Zire 72 camera: a picture file, a V4L2 webcam (Linux desktop) or frames
// a frontend writes into a shared file in the machine folder (Android). The core scales and
// converts; with no source, or a fenced one, the QCI draws its test pattern.
namespace frondCamera {

// The shared frames file, all fields little-endian. The writer makes sequence odd, writes the
// fields and pixels, then makes it even again; the reader copies and checks it didn't move.
// rotation is clockwise degrees (0, 90, 180, 270) applied before the crop; flags bit 0 mirrors
// left to right after rotating, bit 1 marks limited-range YUV (16-235).
constexpr char framesMagic[8] = {'F', 'r', 'o', 'n', 'd', 'C', 'a', 'm'};
constexpr uint32_t framesVersion = 1, framesHeaderBytes = 64;
constexpr uint32_t framesCapacity = 1280 * 960 * 4;
constexpr const char *framesFileName = ".camera-frames";
enum class Format : uint32_t { rgb24 = 1, rgba32 = 2, i420 = 3, nv21 = 4, yuyv = 5 };
constexpr uint32_t flagMirror = 1, flagLimited = 2;
struct FramesHeader {
    char magic[8];
    uint32_t version, capacity, sequence, format, width, height, rotation, flags;
    uint32_t reserved[6];
};
static_assert(sizeof(FramesHeader) == framesHeaderBytes, "frames header is 64 bytes");

// RGB24, rows top down.
struct Picture {
    uint32_t width{}, height{};
    std::vector<uint8_t> rgb;
};

inline uint8_t clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// YCbCr to RGB, BT.601 in 16.16 fixed point: full range as in JFIF (the QCI's inverse), or
// limited range with Y 16-235 and Cb/Cr 16-240.
inline void yccToRgb(int y, int cb, int cr, bool limited, uint8_t *out) {
    int u = cb - 128, v = cr - 128;
    if (limited) {
        y = ((y - 16) * 76309 + 32768) >> 16;
        u = (u * 74711 + 32768) >> 16;  // 255/224
        v = (v * 74711 + 32768) >> 16;
    }
    out[0] = clamp8(y + ((91881 * v + 32768) >> 16));
    out[1] = clamp8(y - ((22554 * u + 46802 * v + 32768) >> 16));
    out[2] = clamp8(y + ((116130 * u + 32768) >> 16));
}

// Bytes a frame of this format and size takes, or 0 when the format is unknown or odd-sized.
inline size_t frameBytes(Format format, uint32_t w, uint32_t h) {
    const size_t n = size_t(w) * h;
    switch (format) {
        case Format::rgb24: return n * 3;
        case Format::rgba32: return n * 4;
        case Format::i420:
        case Format::nv21: return (w & 1) || (h & 1) ? 0 : n + n / 2;
        case Format::yuyv: return (w & 1) ? 0 : n * 2;
    }
    return 0;
}

// Converts a tightly packed frame into a picture, rotated and mirrored. False on a bad size.
inline bool convertFrame(Format format, uint32_t w, uint32_t h, const uint8_t *data, size_t bytes,
                         uint32_t rotation, uint32_t flags, Picture &out) {
    if (!w || !h || w > 4096 || h > 4096 || rotation % 90 || rotation >= 360) return false;
    const size_t need = frameBytes(format, w, h);
    if (!need || bytes < need) return false;
    const bool limited = flags & flagLimited, mirror = flags & flagMirror;
    const bool sideways = rotation == 90 || rotation == 270;
    out.width = sideways ? h : w;
    out.height = sideways ? w : h;
    out.rgb.resize(size_t(w) * h * 3);
    const size_t n = size_t(w) * h;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t px[3];
            const size_t i = size_t(y) * w + x;
            switch (format) {
                case Format::rgb24: memcpy(px, data + i * 3, 3); break;
                case Format::rgba32: memcpy(px, data + i * 4, 3); break;
                case Format::i420: {
                    const size_t c = size_t(y / 2) * (w / 2) + x / 2;
                    yccToRgb(data[i], data[n + c], data[n + n / 4 + c], limited, px);
                    break;
                }
                case Format::nv21: {
                    const size_t c = n + size_t(y / 2) * w + (x & ~1u);
                    yccToRgb(data[i], data[c + 1], data[c], limited, px);
                    break;
                }
                case Format::yuyv: {
                    const uint8_t *pair = data + (i & ~size_t(1)) * 2;
                    yccToRgb(pair[(x & 1) * 2], pair[1], pair[3], limited, px);
                    break;
                }
            }
            // Clockwise rotation, then the mirror, into the output's place.
            uint32_t ox = x, oy = y;
            if (rotation == 90) { ox = h - 1 - y; oy = x; }
            else if (rotation == 180) { ox = w - 1 - x; oy = h - 1 - y; }
            else if (rotation == 270) { ox = y; oy = w - 1 - x; }
            if (mirror) ox = out.width - 1 - ox;
            memcpy(&out.rgb[(size_t(oy) * out.width + ox) * 3], px, 3);
        }
    return true;
}

// Crops the picture's centre to the target's shape, then gives each target pixel the average of
// the source pixels it covers (the nearest one when enlarging).
inline void scalePicture(const Picture &p, uint32_t w, uint32_t h, uint8_t *rgb) {
    uint64_t cw = p.width, ch = p.height;
    if (uint64_t(p.width) * h > uint64_t(p.height) * w) cw = std::max<uint64_t>(1, uint64_t(p.height) * w / h);
    else ch = std::max<uint64_t>(1, uint64_t(p.width) * h / w);
    const uint64_t x0 = (p.width - cw) / 2, y0 = (p.height - ch) / 2;
    std::vector<uint32_t> xs(w + 1);
    for (uint32_t x = 0; x <= w; ++x) xs[x] = uint32_t(x0 + uint64_t(x) * cw / w);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t ya = uint32_t(y0 + uint64_t(y) * ch / h);
        const uint32_t yb = std::max(ya + 1, uint32_t(y0 + uint64_t(y + 1) * ch / h));
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t xa = xs[x], xb = std::max(xa + 1, xs[x + 1]);
            uint32_t sum[3]{}, count = 0;
            for (uint32_t sy = ya; sy < yb; ++sy) {
                const uint8_t *row = &p.rgb[(size_t(sy) * p.width + xa) * 3];
                for (uint32_t sx = xa; sx < xb; ++sx, row += 3, ++count)
                    for (unsigned c = 0; c < 3; ++c) sum[c] += row[c];
            }
            for (unsigned c = 0; c < 3; ++c) rgb[(size_t(y) * w + x) * 3 + c] = uint8_t((sum[c] + count / 2) / count);
        }
    }
}

// Reads a picture file through SDL_image (PNG, JPEG, BMP and the rest it was built with).
inline bool loadPicture(const std::string &path, Picture &out, std::string &error) {
    struct stat st{};
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) { error = "Couldn't read the picture"; return false; }
    SDL_Surface *loaded = IMG_Load(path.c_str());
    if (!loaded) { error = "Couldn't read the picture"; return false; }
    SDL_Surface *rgb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGB24, 0);
    SDL_FreeSurface(loaded);
    if (!rgb) { error = "Couldn't read the picture"; return false; }
    out.width = uint32_t(rgb->w);
    out.height = uint32_t(rgb->h);
    out.rgb.resize(size_t(out.width) * out.height * 3);
    SDL_LockSurface(rgb);
    for (uint32_t y = 0; y < out.height; ++y)
        memcpy(&out.rgb[size_t(y) * out.width * 3], static_cast<uint8_t *>(rgb->pixels) + size_t(y) * rgb->pitch,
               size_t(out.width) * 3);
    SDL_UnlockSurface(rgb);
    SDL_FreeSurface(rgb);
    if (!out.width || !out.height) { error = "Couldn't read the picture"; return false; }
    return true;
}

// The core's side of the frames file: makes it, maps it and reads the newest whole frame.
class FramesFile {
public:
    ~FramesFile() { Close(); }
    // Opens or makes the file, keeping a valid header so a writer's mapping stays good. A link or
    // anything but a plain file is refused. The file stays open so Read can check its size.
    bool Open(const std::string &file, std::string &error) {
        Close();
        const int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { error = "Couldn't make the camera frames file"; return false; }
        const size_t size = framesHeaderBytes + framesCapacity;
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t(st.st_size) != size && ftruncate(fd, off_t(size)))) {
            ::close(fd);
            error = "Couldn't make the camera frames file";
            return false;
        }
        void *at = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (at == MAP_FAILED) { ::close(fd); error = "Couldn't map the camera frames file"; return false; }
        map = static_cast<uint8_t *>(at);
        mapped = fd;
        path = file;
        auto *h = header();
        if (memcmp(h->magic, framesMagic, 8) || h->version != framesVersion || h->capacity != framesCapacity) {
            memset(h, 0, framesHeaderBytes);
            h->version = framesVersion;
            h->capacity = framesCapacity;
            memcpy(h->magic, framesMagic, 8);
        }
        seen = UINT32_MAX;
        return true;
    }
    void Close() {
        if (map) munmap(map, framesHeaderBytes + framesCapacity);
        map = nullptr;
        if (mapped >= 0) ::close(mapped);
        mapped = -1;
    }
    // Deletes the file; for the end of the run.
    void Remove() {
        Close();
        if (!path.empty()) ::unlink(path.c_str());
        path.clear();
    }
    bool IsOpen() const { return map; }
    // Reads the frame there now again on the next Read.
    void Rewind() { seen = UINT32_MAX; }
    const std::string &Path() const { return path; }
    // True when a new whole frame was read into out.
    // A file cut short under the mapping would fault on the copy, so its size is checked first; a writer
    // that truncates it between the check and the copy can still fault, which only another program does.
    bool Read(Picture &out) {
        if (!map) return false;
        struct stat st{};
        if (fstat(mapped, &st) || size_t(st.st_size) < framesHeaderBytes + framesCapacity) return false;
        const uint32_t before = __atomic_load_n(&header()->sequence, __ATOMIC_ACQUIRE);
        if (!before || (before & 1) || before == seen) return false;
        const auto format = Format(load(&header()->format));
        const uint32_t w = load(&header()->width), h = load(&header()->height);
        const uint32_t rotation = load(&header()->rotation), flags = load(&header()->flags);
        const size_t bytes = frameBytes(format, w, h);
        if (!bytes || bytes > framesCapacity) { seen = before; return false; }
        copy.resize(bytes);
        memcpy(copy.data(), map + framesHeaderBytes, bytes);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&header()->sequence, __ATOMIC_RELAXED) != before) return false;
        seen = before;
        return convertFrame(format, w, h, copy.data(), bytes, rotation, flags, out);
    }
private:
    FramesHeader *header() { return reinterpret_cast<FramesHeader *>(map); }
    static uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
    uint8_t *map{};
    int mapped{-1};
    std::string path;
    uint32_t seen{UINT32_MAX};
    std::vector<uint8_t> copy;
};

#ifdef FROND_HOST_V4L2
// A V4L2 capture device in YUYV with memory-mapped buffers, read without blocking.
class Webcam {
public:
    ~Webcam() { Close(); }
    bool Open(const std::string &device, std::string &error) {
        Close();
        // Only a V4L device node (character major 81), checked before opening and again on what was opened.
        struct stat st{};
        if (stat(device.c_str(), &st)) { error = "Couldn't open the webcam"; return false; }
        if (!isV4l(st)) { error = "That device can't capture video"; return false; }
        fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) { error = "Couldn't open the webcam"; return false; }
        struct stat opened{};
        if (fstat(fd, &opened) || !isV4l(opened) || opened.st_rdev != st.st_rdev) {
            error = "That device can't capture video";
            Close();
            return false;
        }
        v4l2_capability cap{};
        const uint32_t caps = !xioctl(VIDIOC_QUERYCAP, &cap) ? 0
                            : (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
            error = "That device can't capture video";
            Close();
            return false;
        }
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.width = 640;
        f.fmt.pix.height = 480;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        if (!xioctl(VIDIOC_S_FMT, &f) || f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
            !f.fmt.pix.width || !f.fmt.pix.height || (f.fmt.pix.width & 1)) {
            error = "The webcam doesn't offer YUYV";
            Close();
            return false;
        }
        width = f.fmt.pix.width;
        height = f.fmt.pix.height;
        stride = std::max<uint32_t>(f.fmt.pix.bytesperline, width * 2);
        limited = f.fmt.pix.quantization != V4L2_QUANTIZATION_FULL_RANGE;
        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (!xioctl(VIDIOC_REQBUFS, &req) || req.count < 2) { error = "The webcam has no capture buffers"; Close(); return false; }
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (!xioctl(VIDIOC_QUERYBUF, &b)) { error = "The webcam has no capture buffers"; Close(); return false; }
            void *at = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
            if (at == MAP_FAILED) { error = "Couldn't map the webcam's buffers"; Close(); return false; }
            buffers.push_back({static_cast<uint8_t *>(at), b.length});
            if (!xioctl(VIDIOC_QBUF, &b)) { error = "Couldn't start the webcam"; Close(); return false; }
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (!xioctl(VIDIOC_STREAMON, &type)) { error = "Couldn't start the webcam"; Close(); return false; }
        streaming = true;
        return true;
    }
    void Close() {
        if (fd >= 0 && streaming) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(VIDIOC_STREAMOFF, &type);
        }
        streaming = false;
        for (auto &b : buffers) munmap(b.at, b.length);
        buffers.clear();
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    bool IsOpen() const { return fd >= 0; }
    // Takes every frame the driver has ready and converts the newest. failed is set when the
    // device went away.
    bool Read(Picture &out, bool &failed) {
        failed = false;
        int newest = -1;
        uint32_t used = 0;
        for (;;) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            if (!xioctl(VIDIOC_DQBUF, &b)) {
                if (errno != EAGAIN) failed = true;
                break;
            }
            if (newest >= 0) requeue(uint32_t(newest));
            newest = int(b.index);
            used = b.bytesused;
            if (b.flags & V4L2_BUF_FLAG_ERROR) { requeue(b.index); newest = -1; }
        }
        if (newest < 0) return false;
        const auto &buffer = buffers[size_t(newest)];
        bool ok = false;
        if (used >= size_t(stride) * (height - 1) + width * 2 && buffer.length >= used) {
            packed.resize(size_t(width) * height * 2);
            for (uint32_t y = 0; y < height; ++y)
                memcpy(&packed[size_t(y) * width * 2], buffer.at + size_t(y) * stride, size_t(width) * 2);
            ok = convertFrame(Format::yuyv, width, height, packed.data(), packed.size(), 0,
                              limited ? flagLimited : 0, out);
        }
        requeue(uint32_t(newest));
        return ok;
    }
private:
    struct Buffer { uint8_t *at; size_t length; };
    static bool isV4l(const struct stat &st) { return S_ISCHR(st.st_mode) && major(st.st_rdev) == 81; }
    bool xioctl(unsigned long request, void *arg) {
        int r;
        do r = ioctl(fd, request, arg); while (r < 0 && errno == EINTR);
        return r >= 0;
    }
    void requeue(uint32_t index) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = index;
        xioctl(VIDIOC_QBUF, &b);
    }
    int fd{-1};
    bool streaming{}, limited{true};
    uint32_t width{}, height{}, stride{};
    std::vector<Buffer> buffers;
    std::vector<uint8_t> packed;
};
#endif

}  // namespace frondCamera

// The camera's host source. Everything but AppEvent runs on the emulator thread. Off (the
// test pattern) at each start; the user picks a source; save, sleep, reset, a load and leaving
// the app put it back to the pattern. Nothing here is session state.
class HostCamera {
public:
    enum class Mode { pattern, picture, webcam, frames };
    ~HostCamera() {
        if (detach) detach();
        Close();
        frames.Remove();
    }
    // Hands the QCI our source; false when the device is asleep. detach takes it back.
    std::function<bool(const CameraSource *)> attach;
    std::function<void()> detach;

    std::string State() const {
        switch (mode) {
            case Mode::picture: return "picture";
            case Mode::webcam: return "webcam";
            case Mode::frames: return "frames";
            case Mode::pattern: break;
        }
        return error.empty() ? "pattern" : "pattern " + error;
    }
    Mode CurrentMode() const { return mode; }

    void Pattern() { Close(); error.clear(); }
    bool Picture(const std::string &path) {
        if (!Prepare()) return false;
        frondCamera::Picture loaded;
        if (!frondCamera::loadPicture(path, loaded, error)) return false;
        picture = std::move(loaded);
        ++version;
        mode = Mode::picture;
        return true;
    }
    bool Webcam(const std::string &device) {
#ifdef FROND_HOST_V4L2
        if (!Prepare()) return false;
        if (!webcam.Open(device, error)) return false;
        picture = {};
        ++version;
        mode = Mode::webcam;
        return true;
#else
        (void)device;
        error = "Webcams aren't supported here";
        return false;
#endif
    }
    // Opens the frames file in folder; its path is in FramesPath() for the writer.
    bool Frames(const std::string &folder) {
        if (folder.empty()) { error = "Frames need a machine folder"; return false; }
        if (!Prepare()) return false;
        const auto file = (std::filesystem::path(folder) / frondCamera::framesFileName).string();
        if (frames.IsOpen() && frames.Path() == file) frames.Rewind();
        else if (!frames.Open(file, error)) return false;
        picture = {};
        ++version;
        mode = Mode::frames;
        return true;
    }
    const std::string &FramesPath() const { return frames.Path(); }

    // Back to the pattern, as for a save.
    void Disarm() { Close(); }
    // Any thread: the app went to the background or its window was hidden.
    void AppEvent(uint32_t type) {
        if (type == SDL_APP_WILLENTERBACKGROUND) background.fetch_add(1);
    }
    // Emulator thread, each loop: drops the source after the app left.
    void Service() {
        const auto seen = background.load();
        if (seen != handled) { handled = seen; Close(); }
    }

    CameraSource Source() {
        return {this,
                [](void *p, uint32_t w, uint32_t h, uint64_t, uint8_t *rgb) {
                    return static_cast<HostCamera *>(p)->Pull(w, h, rgb);
                },
                [](void *p, bool f) { static_cast<HostCamera *>(p)->fenced = f; },
                [](void *p) { static_cast<HostCamera *>(p)->Close(); }};
    }
    // The newest host picture at w x h, or false for the pattern.
    bool Pull(uint32_t w, uint32_t h, uint8_t *rgb) {
        if (fenced || mode == Mode::pattern) return false;
        if (mode == Mode::frames && frames.Read(picture)) ++version;
#ifdef FROND_HOST_V4L2
        if (mode == Mode::webcam) {
            bool failed = false;
            if (webcam.Read(picture, failed)) ++version;
            if (failed) {
                Close();
                error = "Webcam disconnected";
                return false;
            }
        }
#endif
        if (picture.rgb.empty()) return false;
        if (scaledVersion != version || scaledWidth != w || scaledHeight != h) {
            scaled.resize(size_t(w) * h * 3);
            frondCamera::scalePicture(picture, w, h, scaled.data());
            scaledVersion = version;
            scaledWidth = w;
            scaledHeight = h;
        }
        memcpy(rgb, scaled.data(), scaled.size());
        return true;
    }
private:
    // Attaches the source, which also closes whatever ran before; the old source's error goes.
    bool Prepare() {
        error.clear();
        handled = background.load();
        source = Source();
        if (!attach || !attach(&source)) {
            Close();
            error = "Wake the device to use the camera";
            return false;
        }
        Close();
        return true;
    }
    void Close() {
        mode = Mode::pattern;
#ifdef FROND_HOST_V4L2
        webcam.Close();
#endif
        picture = {};
        ++version;
    }
    Mode mode{Mode::pattern};
    std::string error;
    CameraSource source{};
    frondCamera::Picture picture;
    uint64_t version{}, scaledVersion{UINT64_MAX};
    uint32_t scaledWidth{}, scaledHeight{};
    std::vector<uint8_t> scaled;
    frondCamera::FramesFile frames;
#ifdef FROND_HOST_V4L2
    frondCamera::Webcam webcam;
#endif
    std::atomic<uint64_t> background{0};
    uint64_t handled{};
    bool fenced{};
};
#endif
