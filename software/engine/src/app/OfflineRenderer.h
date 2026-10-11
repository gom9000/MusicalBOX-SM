#pragma once

#include <string>
#include <vector>

namespace mbx {

class Engine;

// One entry of the --notes list: a note, or a preset change.
struct ScheduledEvent {
    enum class Kind { Note, Preset, Switch };
    Kind kind = Kind::Note;
    int note = 60;          // Note
    int velocity = 100;     // Note
    int slot = 1;           // Preset, 1..128
    double start = 0.0;     // seconds
    double duration = 1.0;  // seconds, Note only
};

// Parses a comma-separated list, times in seconds:
//   60@0:2        note 60 at 0 s for 2 s (velocity 100)
//   64@0.5:1:80   note 64 at 0.5 s for 1 s, velocity 80
//   p3@1.5        select preset 3 at 1.5 s
//   w@2           press the effect switch at 2 s
bool parseEventList(const std::string& text, std::vector<ScheduledEvent>& out, std::string& error);

// Renders the events through the engine (no audio device) and writes a
// 32-bit float stereo WAV. `tail` seconds are added after the last event.
bool renderOffline(Engine& engine, const std::vector<ScheduledEvent>& events, double tail,
                   const std::string& wavPath, std::string& error);

} // namespace mbx
