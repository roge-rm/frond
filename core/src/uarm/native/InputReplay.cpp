#include "InputReplay.h"
#include "IrKeyboardMapping.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "SdlRenderer.h"
#include "SoC.h"

bool InputReplay::Load(const std::string& path, int width, int height, std::string& error) {
    events.clear();
    snapshots.clear();
    next = 0;
    std::ifstream file(path);
    if (!file) { error = "cannot open input replay: " + path; return false; }
    const std::unordered_map<std::string, KeyId> keys = {
        // Physical app-button positions work across boards; older names are TX aliases.
        {"hard1", keyIdHard1}, {"hard2", keyIdHard2},
        {"hard3", keyIdHard3}, {"hard4", keyIdHard4},
        {"home", keyIdHard1}, {"home-key", keyIdHome}, {"calendar", keyIdHard2}, {"contacts", keyIdHard3},
        {"web", keyIdHard4}, {"up", keyIdUp}, {"down", keyIdDown},
        {"left", keyIdLeft}, {"right", keyIdRight}, {"center", keyIdSelect},
        {"power", keyIdPower}, {"voice", keyIdVoice},
        {"graffiti", keyIdGraffiti}, {"back", keyIdBack}, {"hold", keyIdHold}, {"hotsync", keyIdHotSync},
        {"jog-up", keyIdJogUp}, {"jog-down", keyIdJogDown}, {"jog-press", keyIdJogPress}};
    std::string line;
    size_t number = 0;
    while (std::getline(file, line)) {
        ++number;
        std::istringstream tokens(line);
        std::string time, command, extra;
        if (!(tokens >> time) || time.front() == '#') continue;
        auto fail = [&]() {
            error = path + ":" + std::to_string(number) + ": invalid replay event";
            return false;
        };
        if (!(tokens >> command) || !std::all_of(time.begin(), time.end(),
            [](unsigned char c) { return std::isdigit(c); })) return fail();
        Event event{};
        try { event.at = std::stoull(time); } catch (...) { return fail(); }
        if (!events.empty() && event.at < events.back().at) return fail();
        if (command == "pen-down" || command == "pen-move") {
            event.kind = Kind::penDown;
            if (!(tokens >> event.x >> event.y) || event.x < 0 || event.x >= width ||
                event.y < 0 || event.y >= height) return fail();
        } else if (command == "pen-up") {
            event.kind = Kind::penUp;
        } else if (command == "key-down" || command == "key-up") {
            std::string key;
            if (!(tokens >> key)) return fail();
            const auto named = keys.find(key);
            if (named != keys.end()) event.key = named->second;
            else {
                if (key.empty() || !std::all_of(key.begin(), key.end(),
                    [](unsigned char c) { return std::isdigit(c); })) return fail();
                uint64_t id{};
                try { id = std::stoull(key); } catch (...) { return fail(); }
                if (!id || id >= keyIdCount) return fail();
                event.key = static_cast<KeyId>(id);
            }
            event.kind = command == "key-down" ? Kind::keyDown : Kind::keyUp;
        } else if (command == "keyboard-down" || command == "keyboard-up") {
            std::string name;
            if (!(tokens >> std::quoted(name))) return fail();
            event.x=irKeyboardCode(SDL_GetScancodeFromName(name.c_str()));
            if (!event.x) return fail();
            event.kind=command=="keyboard-down"?Kind::keyboardDown:Kind::keyboardUp;
        } else if (command == "keyboard-attach" || command == "keyboard-detach") {
            event.kind = Kind::keyboardAttach;
            event.x = command == "keyboard-attach";
        } else if (command == "stick") {
            // The analogue stick in hundredths of its travel, right and down positive.
            event.kind = Kind::stick;
            if (!(tokens >> event.x >> event.y) || event.x < -100 || event.x > 100 || event.y < -100 || event.y > 100)
                return fail();
        } else if (command == "snapshot") {
            event.kind = Kind::snapshot;
            if (!(tokens >> std::quoted(event.path)) || event.path.empty()) return fail();
        } else { return fail(); }
        if ((tokens >> extra) && extra.front() != '#') return fail();
        events.push_back(event);
    }
    if (file.bad()) { error = "failed reading input replay: " + path; return false; }
    return true;
}

void InputReplay::Dispatch(uint64_t elapsedMs, SoC& soc, SdlRenderer& renderer) {
    Dispatch(elapsedMs, soc, [&](const char* path, bool& blank) { return renderer.SaveFramebuffer(path, blank); });
}

void InputReplay::Dispatch(uint64_t elapsedMs, SoC& soc, const std::function<bool(const char*, bool&)>& save) {
    while (next < events.size() && events[next].at <= elapsedMs) {
        const auto& event = events[next++];
        switch (event.kind) {
            case Kind::penDown: soc.PenDown(event.x, event.y); break;
            case Kind::penUp: soc.PenUp(); break;
            case Kind::keyDown: soc.KeyDown(event.key); break;
            case Kind::keyUp: soc.KeyUp(event.key); break;
            case Kind::keyboardDown: soc.KeyboardKey(event.x,true); break;
            case Kind::keyboardUp: soc.KeyboardKey(event.x,false); break;
            case Kind::keyboardAttach: soc.AttachKeyboard(event.x); break;
            case Kind::stick: soc.AnalogStick(event.x / 100.0f, event.y / 100.0f); break;
            case Kind::snapshot: {
                // A failed snapshot is reported but never ends the run.
                bool blank = false;
                if (!save(event.path.c_str(), blank)) {
                    snapshots.push_back("failed");
                    std::cerr << "cannot capture replay snapshot: " << event.path << std::endl;
                } else {
                    const char* result = !blank ? "ok" : soc.LcdEnabled() ? "no-frame" : "lcd-off";
                    snapshots.push_back(result);
                    if (blank) std::cout << "replay snapshot " << result << ", saved a black frame: "
                                         << event.path << std::endl;
                }
                break;
            }
        }
        std::cout << "replay event " << next << " at " << elapsedMs << " ms" << std::endl;
    }
}
