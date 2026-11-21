#ifndef FROND_PWI_NOTE_H
#define FROND_PWI_NOTE_H

#include <optional>
#include <string>
#include <string_view>

#include "PalmDb.h"

namespace frondSync {

// Reads the observed WM5 Notes default-font single-line ASCII format.
std::optional<std::string> ReadPwiNote(const Bytes& data);

// Builds a default-font note from 1 to 21 printable ASCII characters.
std::optional<Bytes> WritePwiNote(std::string_view text);

// Replaces a recognised note with a supported line, leaving it intact on failure.
bool ReplacePwiNoteText(Bytes& data, std::string_view text);

}  // namespace frondSync

#endif
