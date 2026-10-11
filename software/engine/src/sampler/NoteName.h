#pragma once

#include <string>
#include <string_view>

namespace mbx {

// Note names use the C3 = 60 convention (Yamaha, SamplerBox):
// "C3" -> 60, "C#3" -> 61, "Db3" -> 61, "A-1" -> 21.
// Returns -1 if the text is not a valid note name in 0..127.
int parseNoteName(std::string_view text);

// Inverse of parseNoteName, using sharps: 61 -> "C#3".
std::string noteName(int midiNote);

} // namespace mbx
