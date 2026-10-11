#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mbx {

// One WAV file in RAM, mapped to a root note and a velocity range.
// Audio is kept as 16-bit integers (interleaved when stereo) to halve the
// memory compared with float; it is converted on the fly by Voice.
struct Zone {
    std::vector<std::int16_t> data;
    std::uint32_t frames = 0;
    int channels = 1;            // 1 or 2
    double sampleRate = 48000.0; // of the file; Voice compensates

    int rootNote = 60;           // MIDI note recorded in the file (C3 = 60)
    int loVel = 0;               // velocity range served by this zone
    int hiVel = 127;

    bool hasLoop = false;        // loop points read from the WAV 'smpl' chunk
    std::uint32_t loopStart = 0; // first frame of the loop
    std::uint32_t loopEnd = 0;   // last frame of the loop (inclusive)

    bool dualMono = false;       // stereo file with identical channels, kept as mono
    std::string file;            // for diagnostics only

    std::size_t memoryBytes() const { return data.size() * sizeof(std::int16_t); }
};

} // namespace mbx
