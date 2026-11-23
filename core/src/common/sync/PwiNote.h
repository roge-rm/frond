#ifndef FROND_PWI_NOTE_H
#define FROND_PWI_NOTE_H

#include <optional>
#include <string>
#include <string_view>

#include "PalmDb.h"

namespace frondSync {

struct PimRecord;

inline constexpr char noteFileField[] = "wm5-note-file";
inline constexpr char noteTextField[] = "wm5-note-text";
void DetachPwiMemo(PimRecord& memo);

// Reads the observed WM5 Notes default-font plain text format.
std::optional<std::string> ReadPwiNote(const Bytes& data);

// Builds a default-font note from printable ASCII, Latin-1, the euro sign and line breaks.
std::optional<Bytes> WritePwiNote(std::string_view text);

// Replaces a recognised note with new text, leaving it intact on failure.
bool ReplacePwiNoteText(Bytes& data, std::string_view text);

}  // namespace frondSync

#endif
