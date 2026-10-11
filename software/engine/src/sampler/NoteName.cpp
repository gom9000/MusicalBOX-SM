#include "sampler/NoteName.h"

#include <cctype>

namespace mbx {

int parseNoteName(std::string_view text) {
    if (text.empty()) return -1;

    // Semitone of each letter, indexed from 'A'.
    static constexpr int kLetter[7] = {9, 11, 0, 2, 4, 5, 7};
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (letter < 'A' || letter > 'G') return -1;
    int semitone = kLetter[letter - 'A'];

    std::size_t i = 1;
    auto octaveFollows = [&](std::size_t at) {
        return at < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '-');
    };
    if (i < text.size() && text[i] == '#') {
        ++semitone;
        ++i;
    } else if (i < text.size() && text[i] == 'b' && octaveFollows(i + 1)) {
        --semitone;
        ++i;
    }

    if (!octaveFollows(i)) return -1;
    bool negative = false;
    if (text[i] == '-') {
        negative = true;
        ++i;
    }
    if (i >= text.size()) return -1;
    int octave = 0;
    for (; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return -1;
        octave = octave * 10 + (text[i] - '0');
        if (octave > 20) return -1;
    }
    if (negative) octave = -octave;

    const int midi = semitone + (octave + 2) * 12;
    return (midi >= 0 && midi <= 127) ? midi : -1;
}

std::string noteName(int midiNote) {
    static constexpr const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                               "F#", "G", "G#", "A", "A#", "B"};
    if (midiNote < 0 || midiNote > 127) return "?";
    return std::string(kNames[midiNote % 12]) + std::to_string(midiNote / 12 - 2);
}

} // namespace mbx
