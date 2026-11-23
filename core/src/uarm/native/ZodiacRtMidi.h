#ifndef FROND_ZODIAC_RT_MIDI_H
#define FROND_ZODIAC_RT_MIDI_H

// Desktop MIDI output ports for the Zodiac's music, through RtMidi (vendor/rtmidi): ALSA sequencer on Linux,
// CoreMIDI on macOS, WinMM on Windows. Not built on Android, which uses MidiManager.
#include <memory>
#include <string>
#include <vector>

#include "ZodiacMidiOut.h"

// The output ports' names, in index order. False with error set when there's no MIDI system to ask.
bool zodiacMidiPorts(std::vector<std::string>& names, std::string& error);

// A port's name without the numbers that change with plug order: ALSA's trailing client:port (" 128:0") and
// any number in brackets, like the PID FluidSynth adds ("FLUID Synth (4321)", "(4321:0)"), in lower case.
inline std::string zodiacMidiPortKey(const std::string& name) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    // "N" or "N:N" from i to end, digits on both sides of the colon.
    const auto numbers = [&](size_t i, size_t end) {
        size_t j = i;
        while (j < end && digit(name[j])) ++j;
        if (j == i) return false;
        if (j == end) return true;
        if (name[j] != ':') return false;
        const size_t k = ++j;
        while (j < end && digit(name[j])) ++j;
        return j > k && j == end;
    };
    std::string s;
    size_t end = name.size();
    // The trailing client:port.
    const size_t space = name.rfind(' ');
    if (space != std::string::npos && name.find(':', space) != std::string::npos && numbers(space + 1, end)) end = space;
    for (size_t i = 0; i < end; ++i) {
        if (name[i] == '(') {
            const size_t close = name.find(')', i);
            if (close != std::string::npos && close < end && numbers(i + 1, close)) {
                if (!s.empty() && s.back() == ' ') s.pop_back();
                i = close;
                continue;
            }
        }
        s += char(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Opens an output: "virtual" makes a port of Frond's own for other programs to connect to (not on Windows), a
// number is a port's index, anything else a port by name: the same name first, then the same name without the
// numbers that change with plug order (zodiacMidiPortKey), then the first whose name contains it. Null with error
// set when it can't; opened is the port's name.
std::unique_ptr<ZodiacMidiOut::Port> zodiacOpenMidiPort(const std::string& spec, std::string& opened, std::string& error);

#endif
