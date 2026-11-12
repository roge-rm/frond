// Firmware-free host sources for the Zire 72 camera (native/HostCamera.h): a picture file and
// frames pushed through the shared frames file, through the real SoC's QCI and DMA at each
// sensor size, plus the conversions, the crop and scale, and the source's lifecycle.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "soc_PXA.h"
#include "camera_input.h"
#include "mem.h"
#include "HostCamera.h"

namespace {
constexpr uint32_t I2C = 0x40301680, IDBR = I2C + 8, ICR = I2C + 0x10, ISR = I2C + 0x18;
constexpr uint32_t GPIO = 0x40e00000;
constexpr uint32_t QCI = 0x50000000, CICR0 = QCI, CICR1 = QCI + 4, CICR3 = QCI + 0xc,
                   CICR4 = QCI + 0x10, CIFR = QCI + 0x18, CIBR0 = QCI + 0x28;
constexpr uint32_t DMA = 0x40000000;
constexpr uint32_t ROM_CICR1 = 0x009f8412, ROM_CICR4 = 0x880001;

struct Machine {
    std::vector<uint8_t> rom;
    SocPXA soc;
    Machine() : rom(4096), soc(deviceTypeZire72, 32u << 20, rom.data(), uint32_t(rom.size()), nullptr, 0, -1, 2) {
        wr(0xa0003000, 0xeafffffe);
        cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
        cpuSetReg(soc.GetCpu(), 15, 0xa0003000);
    }
    void wr(uint32_t a, uint32_t v) { assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, true, &v)); }
    uint32_t rd(uint32_t a) { uint32_t v = 0; assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, false, &v)); return v; }
    uint8_t rd8(uint32_t a) { uint8_t v = 0; assert(memAccess(cpuGetMem(soc.GetCpu()), a, 1, false, &v)); return v; }
    void run(unsigned ms) { soc.Run(uint64_t(ms) * 100000, 100000000); }
    void pins() {
        wr(GPIO + 0x10, rd(GPIO + 0x10) | 3u << 24);
        wr(GPIO + 0x14, rd(GPIO + 0x14) | 1u << 27);
        wr(GPIO + 0x2c, 1u << 27);  // supply on (low)
        wr(GPIO + 0x28, 1u << 25);  // RESET low
        wr(GPIO + 0x28, 1u << 24);  // PWDN low
    }
    bool send(uint8_t byte, uint32_t extra) {
        wr(IDBR, byte);
        wr(ICR, 0x48 | extra);
        return !(rd(ISR) & 2);
    }
    void sccbWrite(uint8_t reg, uint8_t value) {
        assert(send(0x60, 1));
        send(reg, 0);
        send(value, 2);
    }
    // The sensor at w x h in UYVY and the QCI's ROM setup with that window.
    void start(unsigned w, unsigned h) {
        pins();
        sccbWrite(0x12, 0x80);
        const uint8_t com7 = w == 1280 ? 0x00 : w == 640 ? 0x40 : 0x10;
        sccbWrite(0x12, com7);
        if (w == 160) sccbWrite(0x04, 0x20);  // COM1: QQVGA
        sccbWrite(0x11, 0x80);
        sccbWrite(0x3a, 0x08);
        wr(CIFR, 0x0f);
        wr(CICR4, ROM_CICR4);
        wr(CICR1, (ROM_CICR1 & ~(0x7ffu << 15)) | (w - 1) << 15);
        wr(CICR3, h - 1);
    }
};

void chain(Machine &m, unsigned channel, uint32_t descriptors, uint32_t dest, uint32_t bytes) {
    const uint32_t source = CIBR0 + 8 * channel;
    for (uint32_t done = 0, at = descriptors; done < bytes; done += 8000, at += 16) {
        const uint32_t n = std::min<uint32_t>(8000, bytes - done);
        m.wr(at, done + n < bytes ? at + 16 : 1);
        m.wr(at + 4, source);
        m.wr(at + 8, dest + done);
        m.wr(at + 12, 0x60030000 | n);
    }
    m.wr(DMA + 0x1110 + 4 * channel, 0x80 | channel);
    m.wr(DMA + 0x200 + 16 * channel, descriptors);
    m.wr(DMA + 4 * channel, 0x80000000);
}

int ycc(const uint8_t *p, int which) {
    const int r = p[0], g = p[1], b = p[2];
    if (which == 0) return (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
    if (which == 1) return std::clamp(((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16) + 128, 0, 255);
    return std::clamp(((32768 * r - 27439 * g - 5329 * b + 32768) >> 16) + 128, 0, 255);
}

// The planes the QCI gives for this RGB frame in planar YCbCr 4:2:2.
void planes(const std::vector<uint8_t> &rgb, unsigned w, unsigned h, std::vector<uint8_t> &y,
            std::vector<uint8_t> &cb, std::vector<uint8_t> &cr) {
    y.clear(); cb.clear(); cr.clear();
    for (unsigned i = 0; i < w * h; i += 2) {
        int u = 0, v = 0;
        for (unsigned k = 0; k < 2; ++k) {
            y.push_back(uint8_t(ycc(&rgb[(i + k) * 3], 0)));
            u += ycc(&rgb[(i + k) * 3], 1);
            v += ycc(&rgb[(i + k) * 3], 2);
        }
        cb.push_back(uint8_t((u + 1) / 2));
        cr.push_back(uint8_t((v + 1) / 2));
    }
}

bool equal(Machine &m, uint32_t at, const std::vector<uint8_t> &want) {
    for (size_t i = 0; i < want.size(); ++i)
        if (m.rd8(at + uint32_t(i)) != want[i]) {
            printf("byte %zu at %08x: %u, want %u\n", i, at, m.rd8(at + uint32_t(i)), want[i]);
            return false;
        }
    return true;
}

// One whole frame through DMA at w x h; the planes in RAM afterwards.
void capture(Machine &m, unsigned w, unsigned h) {
    chain(m, 0, 0xa0100000, 0xa0200000, w * h);
    chain(m, 1, 0xa0110000, 0xa0400000, w * h / 2);
    chain(m, 2, 0xa0120000, 0xa0500000, w * h / 2);
    m.wr(CICR0, 0x900003ff);
    for (unsigned i = 0; i < 1000 && (m.rd(DMA) & 8) == 0; ++i) m.run(10);
    for (unsigned c = 0; c < 3; ++c) assert((m.rd(DMA + 4 * c) & 9) == 8);
    m.wr(CICR0, 0);
}

// 1280 x 960 in 160-pixel blocks of eight colours by column, darker by row.
frondCamera::Picture blocks() {
    frondCamera::Picture p{1280, 960, std::vector<uint8_t>(1280 * 960 * 3)};
    for (unsigned y = 0; y < 960; ++y)
        for (unsigned x = 0; x < 1280; ++x)
            for (unsigned c = 0; c < 3; ++c)
                p.rgb[(y * 1280 + x) * 3 + c] = uint8_t(cameraTestPatternBars[x / 160][c] * (6 - y / 160) / 6);
    return p;
}

// The pushed-frames writer, as a frontend does it: sequence odd, fields and pixels, then even.
struct Writer {
    uint8_t *map{};
    explicit Writer(const std::string &path) {
        const int fd = open(path.c_str(), O_RDWR);
        assert(fd >= 0);
        map = static_cast<uint8_t *>(mmap(nullptr, frondCamera::framesHeaderBytes + frondCamera::framesCapacity,
                                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        close(fd);
        assert(map != MAP_FAILED);
        assert(!memcmp(map, frondCamera::framesMagic, 8));
    }
    ~Writer() { munmap(map, frondCamera::framesHeaderBytes + frondCamera::framesCapacity); }
    auto *header() { return reinterpret_cast<frondCamera::FramesHeader *>(map); }
    void begin() { header()->sequence |= 1; }
    void end() { header()->sequence += 1; }
    void write(frondCamera::Format f, uint32_t w, uint32_t h, const std::vector<uint8_t> &data, uint32_t rotation = 0,
               uint32_t flags = 0) {
        begin();
        header()->format = uint32_t(f); header()->width = w; header()->height = h;
        header()->rotation = rotation; header()->flags = flags;
        memcpy(map + frondCamera::framesHeaderBytes, data.data(), data.size());
        end();
    }
};
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto dir = std::filesystem::temp_directory_path() / ("frond-camera-host-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);

    // Conversions: the YUV formats give the colour back, rotations turn the frame clockwise.
    {
        const uint8_t pixel[3] = {200, 40, 90};
        const int y = ycc(pixel, 0), u = ycc(pixel, 1), v = ycc(pixel, 2);
        std::vector<uint8_t> i420(4 * 2 + 2 * 2), nv21(4 * 2 + 4), yuyv(4 * 2 * 2);
        std::fill(i420.begin(), i420.begin() + 8, uint8_t(y));
        i420[8] = i420[9] = uint8_t(u); i420[10] = i420[11] = uint8_t(v);
        std::fill(nv21.begin(), nv21.begin() + 8, uint8_t(y));
        nv21[8] = nv21[10] = uint8_t(v); nv21[9] = nv21[11] = uint8_t(u);
        for (unsigned i = 0; i < 4; ++i) { yuyv[i * 4] = yuyv[i * 4 + 2] = uint8_t(y); yuyv[i * 4 + 1] = uint8_t(u); yuyv[i * 4 + 3] = uint8_t(v); }
        frondCamera::Picture out;
        for (auto [f, data] : {std::pair{frondCamera::Format::i420, &i420}, std::pair{frondCamera::Format::nv21, &nv21},
                               std::pair{frondCamera::Format::yuyv, &yuyv}}) {
            assert(frondCamera::convertFrame(f, 4, 2, data->data(), data->size(), 0, 0, out));
            for (unsigned i = 0; i < 8; ++i)
                for (unsigned c = 0; c < 3; ++c) assert(std::abs(out.rgb[i * 3 + c] - pixel[c]) <= 2);
        }
        // Limited range: Y 16 is black and 235 white.
        std::vector<uint8_t> limited = {16, 128, 16, 128, 235, 128, 235, 128};
        assert(frondCamera::convertFrame(frondCamera::Format::yuyv, 4, 1, limited.data(), limited.size(), 0,
                                         frondCamera::flagLimited, out));
        assert(out.rgb[0] == 0 && out.rgb[6] == 255 && out.rgb[11] == 255);
        // A 3 x 2 RGB frame turned 90 degrees is 2 x 3 with its bottom-left pixel at the top left.
        std::vector<uint8_t> rgb(18);
        for (unsigned i = 0; i < 6; ++i) rgb[i * 3] = uint8_t(i);
        assert(frondCamera::convertFrame(frondCamera::Format::rgb24, 3, 2, rgb.data(), rgb.size(), 90, 0, out));
        assert(out.width == 2 && out.height == 3);
        const uint8_t turned[6] = {3, 0, 4, 1, 5, 2};
        for (unsigned i = 0; i < 6; ++i) assert(out.rgb[i * 3] == turned[i]);
        assert(frondCamera::convertFrame(frondCamera::Format::rgb24, 3, 2, rgb.data(), rgb.size(), 270, 0, out));
        const uint8_t back[6] = {2, 5, 1, 4, 0, 3};
        for (unsigned i = 0; i < 6; ++i) assert(out.rgb[i * 3] == back[i]);
        assert(frondCamera::convertFrame(frondCamera::Format::rgb24, 3, 2, rgb.data(), rgb.size(), 180, frondCamera::flagMirror, out));
        const uint8_t flipped[6] = {3, 4, 5, 0, 1, 2};
        for (unsigned i = 0; i < 6; ++i) assert(out.rgb[i * 3] == flipped[i]);
        // Bad sizes and rotations are refused.
        assert(!frondCamera::convertFrame(frondCamera::Format::rgb24, 3, 2, rgb.data(), 17, 0, 0, out));
        assert(!frondCamera::convertFrame(frondCamera::Format::rgb24, 3, 2, rgb.data(), rgb.size(), 45, 0, out));
        assert(!frondCamera::convertFrame(frondCamera::Format::i420, 3, 2, rgb.data(), rgb.size(), 0, 0, out));
        assert(!frondCamera::convertFrame(frondCamera::Format(9), 3, 2, rgb.data(), rgb.size(), 0, 0, out));
    }

    // Scaling: an average over each target pixel's area, a centre crop to 4:3, nearest when enlarging.
    {
        frondCamera::Picture p{4, 2, {0, 0, 0, 100, 100, 100, 50, 50, 50, 250, 250, 250,
                                      0, 0, 0, 100, 100, 100, 50, 50, 50, 250, 250, 250}};
        uint8_t out[6];
        frondCamera::scalePicture(p, 2, 1, out);
        assert(out[0] == 50 && out[3] == 150);
        // 16 x 6 cropped to 8 x 6: columns 4-11.
        frondCamera::Picture wide{16, 6, std::vector<uint8_t>(16 * 6 * 3)};
        for (unsigned y = 0; y < 6; ++y)
            for (unsigned x = 0; x < 16; ++x) wide.rgb[(y * 16 + x) * 3] = uint8_t(x);
        uint8_t row[8 * 6 * 3];
        frondCamera::scalePicture(wide, 8, 6, row);
        for (unsigned x = 0; x < 8; ++x) assert(row[x * 3] == x + 4);
        uint8_t big[16 * 3 * 3];
        frondCamera::scalePicture(p, 16, 3, big);  // a wider shape keeps one row, enlarged
        assert(big[0] == 0 && big[4 * 3] == 100 && big[15 * 3] == 250 && big[(2 * 16 + 15) * 3] == 250);
    }

    const auto picture = blocks();
    const auto png = (dir / "blocks.png").string();
    {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t *>(picture.rgb.data()), 1280, 960, 24,
                                                            1280 * 3, SDL_PIXELFORMAT_RGB24);
        assert(s && IMG_SavePNG(s, png.c_str()) == 0);
        SDL_FreeSurface(s);
    }

    // A picture file through the QCI at each sensor size: byte for byte what the core's scale
    // gives, and each block's centre carries the block's colour.
    for (auto [w, h] : {std::pair{160u, 120u}, std::pair{320u, 240u}, std::pair{640u, 480u}, std::pair{1280u, 960u}}) {
        Machine m;
        HostCamera camera;
        camera.attach = [&](const CameraSource *s) { m.soc.SetCameraSource(s); return true; };
        camera.detach = [&] { m.soc.SetCameraSource(nullptr); };
        assert(camera.Picture(png) && camera.State() == "picture");
        m.start(w, h);
        capture(m, w, h);
        std::vector<uint8_t> rgb(w * h * 3), y, cb, cr;
        frondCamera::scalePicture(picture, w, h, rgb.data());
        planes(rgb, w, h, y, cb, cr);
        assert(equal(m, 0xa0200000, y) && equal(m, 0xa0400000, cb) && equal(m, 0xa0500000, cr));
        for (unsigned by = 0; by < 6; ++by)
            for (unsigned bx = 0; bx < 8; ++bx) {
                const unsigned x = (bx * 2 + 1) * w / 16, yy = (by * 2 + 1) * h / 12;
                const uint8_t *want = &picture.rgb[(((by * 2 + 1) * 80) * 1280 + (bx * 2 + 1) * 80) * 3];
                assert(m.rd8(0xa0200000 + yy * w + x) == ycc(want, 0));
            }
        printf("picture %u x %u: ok\n", w, h);
    }

    // A missing or unreadable picture leaves the pattern and says why.
    {
        Machine m;
        HostCamera camera;
        camera.attach = [&](const CameraSource *s) { m.soc.SetCameraSource(s); return true; };
        camera.detach = [&] { m.soc.SetCameraSource(nullptr); };
        assert(!camera.Picture((dir / "missing.png").string()));
        assert(camera.State() == "pattern Couldn't read the picture");
        camera.Pattern();
        assert(camera.State() == "pattern");
        assert(!camera.Frames(""));
        assert(camera.State() == "pattern Frames need a machine folder");
        // Asleep: refused.
        camera.attach = [](const CameraSource *) { return false; };
        assert(!camera.Picture(png) && camera.State() == "pattern Wake the device to use the camera");
    }

    // Pushed frames: the file has a header, frames show at the QCI, a frame being written is
    // skipped and the last whole one stays, and lifecycle events go back to the pattern.
    {
        Machine m;
        HostCamera camera;
        camera.attach = [&](const CameraSource *s) { m.soc.SetCameraSource(s); return true; };
        camera.detach = [&] { m.soc.SetCameraSource(nullptr); };
        assert(camera.Frames(dir.string()) && camera.State() == "frames");
        const auto path = camera.FramesPath();
        assert(path == (dir / ".camera-frames").string());
        Writer writer(path);
        std::vector<uint8_t> out(320 * 240 * 3);
        assert(!camera.Pull(320, 240, out.data()));  // nothing written yet: the pattern

        // 640 x 480 RGBA of the blocks, scaled down by the core.
        std::vector<uint8_t> rgba(640 * 480 * 4), small(640 * 480 * 3);
        frondCamera::scalePicture(picture, 640, 480, small.data());
        for (unsigned i = 0; i < 640 * 480; ++i) { memcpy(&rgba[i * 4], &small[i * 3], 3); rgba[i * 4 + 3] = 255; }
        writer.write(frondCamera::Format::rgba32, 640, 480, rgba);
        m.start(320, 240);
        capture(m, 320, 240);
        frondCamera::Picture from{640, 480, small};
        std::vector<uint8_t> rgb(320 * 240 * 3), y, cb, cr;
        frondCamera::scalePicture(from, 320, 240, rgb.data());
        planes(rgb, 320, 240, y, cb, cr);
        assert(equal(m, 0xa0200000, y) && equal(m, 0xa0400000, cb) && equal(m, 0xa0500000, cr));

        // A phone frame: I420, sideways (rotation 90) and 480 x 640 after turning.
        std::vector<uint8_t> i420(640 * 480 * 3 / 2, 128);
        std::fill(i420.begin(), i420.begin() + 640 * 480, uint8_t(200));
        writer.write(frondCamera::Format::i420, 640, 480, i420, 90);
        assert(camera.Pull(320, 240, out.data()));
        assert(out[0] == 200 && out[1] == 200 && out[2] == 200);
        // Mid-write: the last whole frame stays.
        writer.begin();
        std::fill(writer.map + frondCamera::framesHeaderBytes, writer.map + frondCamera::framesHeaderBytes + 640 * 480, uint8_t(10));
        assert(camera.Pull(320, 240, out.data()) && out[0] == 200);
        writer.end();
        assert(camera.Pull(320, 240, out.data()) && out[0] == 10);
        // A frame too big for the file is ignored.
        writer.begin(); writer.header()->format = 2; writer.header()->width = 4096; writer.header()->height = 4096; writer.end();
        assert(camera.Pull(320, 240, out.data()) && out[0] == 10);

        // Fenced in a checkpoint: the pattern meanwhile.
        {
            SoC::InputCheckpoint checkpoint(m.soc);
            assert(!camera.Pull(320, 240, out.data()));
        }
        assert(camera.Pull(320, 240, out.data()));
        // Disarm (a save) and the pattern; arming again reads the frame there now.
        camera.Disarm();
        assert(camera.State() == "pattern" && !camera.Pull(320, 240, out.data()));
        writer.write(frondCamera::Format::i420, 640, 480, i420, 90);
        assert(camera.Frames(dir.string()) && camera.FramesPath() == path);
        assert(camera.Pull(320, 240, out.data()) && out[0] == 200);
        // Leaving the app: the next service drops it.
        camera.AppEvent(SDL_APP_WILLENTERBACKGROUND);
        camera.Service();
        assert(camera.State() == "pattern");
        // A load drops the source through the SoC, as sleep and reset do.
        assert(camera.Frames(dir.string()));
        assert(m.soc.Save());
        auto saved = m.soc.GetSavestate();
        std::vector<uint8_t> copy((uint8_t *)saved.data, (uint8_t *)saved.data + saved.size);
        assert(m.soc.Load(copy.size(), copy.data()));
        assert(camera.State() == "pattern");
    }
    // The frames file goes with the camera.
    assert(!std::filesystem::exists(dir / ".camera-frames"));

    // A new picture source replaces the old one.
    {
        Machine m;
        HostCamera camera;
        camera.attach = [&](const CameraSource *s) { m.soc.SetCameraSource(s); return true; };
        camera.detach = [&] { m.soc.SetCameraSource(nullptr); };
        assert(camera.Frames(dir.string()));
        assert(camera.Picture(png) && camera.State() == "picture");
        std::vector<uint8_t> out(1280 * 960 * 3);
        assert(camera.Pull(1280, 960, out.data()) && out == picture.rgb);
    }
    std::filesystem::remove_all(dir);
    printf("z72-camera-host: ok\n");
}
