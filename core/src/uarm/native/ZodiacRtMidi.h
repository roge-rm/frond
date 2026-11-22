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

// Opens an output: "virtual" makes a port of Frond's own for other programs to connect to (not on Windows), a
// number is a port's index, anything else the first port whose name contains it. Null with error set when it
// can't; opened is the port's name.
std::unique_ptr<ZodiacMidiOut::Port> zodiacOpenMidiPort(const std::string& spec, std::string& opened, std::string& error);

#endif
