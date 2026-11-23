#include "ZodiacRtMidi.h"

#include <atomic>
#include <cctype>
#include <cstdlib>

// RtMidi's back end for this platform, then RtMidi itself in this file alone.
#if defined(__APPLE__)
#define __MACOSX_CORE__
#elif defined(_WIN32)
#define __WINDOWS_MM__
// winshim/windows.h takes far and near away, and mmsystem.h still uses them.
#include <windows.h>
#define far
#define near
#elif defined(__linux__) && !defined(__ANDROID__)
#define __LINUX_ALSA__
#endif
#include "../../../../vendor/rtmidi/RtMidi.cpp"
#if defined(_WIN32)
#undef far
#undef near
#endif

namespace {
const char* const virtualName = "Frond Zodiac music";

class RtPort : public ZodiacMidiOut::Port {
   public:
    explicit RtPort(std::unique_ptr<RtMidiOut> out) : out_(std::move(out)) {}
    // Sent as soon as it's given: RtMidi has no timestamps, and the engine gives messages when they're due.
    void send(uint64_t, const uint8_t* bytes, size_t size) override {
        try {
            out_->sendMessage(bytes, size);
        } catch (const RtMidiError&) {
            // A port that went away: the music goes back to the FM, the emulator goes on.
            failed_.store(true, std::memory_order_relaxed);
        }
    }
    bool failed() const override { return failed_.load(std::memory_order_relaxed); }

   private:
    std::unique_ptr<RtMidiOut> out_;
    std::atomic<bool> failed_{false};
};

std::unique_ptr<RtMidiOut> makeOut(std::string& error) {
    try {
        // With no error callback RtMidi throws its errors and prints its warnings on stderr.
        return std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, "Frond");
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        return nullptr;
    }
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

bool zodiacMidiPorts(std::vector<std::string>& names, std::string& error) {
    names.clear();
    auto out = makeOut(error);
    if (!out) return false;
    const unsigned count = out->getPortCount();
    for (unsigned i = 0; i < count; ++i) names.push_back(out->getPortName(i));
    return true;
}

std::unique_ptr<ZodiacMidiOut::Port> zodiacOpenMidiPort(const std::string& spec, std::string& opened, std::string& error) {
    auto out = makeOut(error);
    if (!out) return nullptr;
    try {
        if (spec == "virtual") {
#if defined(_WIN32)
            error = "Windows has no virtual MIDI ports";
            return nullptr;
#else
            out->openVirtualPort(virtualName);
            opened = virtualName;
            return std::make_unique<RtPort>(std::move(out));
#endif
        }
        const unsigned count = out->getPortCount();
        char* end = nullptr;
        const unsigned long index = std::strtoul(spec.c_str(), &end, 10);
        const bool number = !spec.empty() && end && !*end;
        unsigned found = count;
        if (number) {
            found = index < count ? unsigned(index) : count;
        } else {
            std::vector<std::string> names;
            for (unsigned i = 0; i < count; ++i) names.push_back(out->getPortName(i));
            const std::string key = zodiacMidiPortKey(spec);
            for (unsigned i = 0; i < count && found == count; ++i)
                if (names[i] == spec) found = i;
            for (unsigned i = 0; i < count && found == count; ++i)
                if (zodiacMidiPortKey(names[i]) == key) found = i;
            for (unsigned i = 0; i < count && found == count; ++i)
                if (!key.empty() && zodiacMidiPortKey(names[i]).find(key) != std::string::npos) found = i;
            for (unsigned i = 0; i < count && found == count; ++i)
                if (lower(names[i]).find(lower(spec)) != std::string::npos) found = i;
        }
        if (found == count) {
            error = count ? "no MIDI output port " + spec : "no MIDI output ports";
            return nullptr;
        }
        opened = out->getPortName(found);
        out->openPort(found, "Frond Zodiac music");
        return std::make_unique<RtPort>(std::move(out));
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        return nullptr;
    }
}
