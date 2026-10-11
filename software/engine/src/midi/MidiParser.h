#pragma once

#include "midi/MidiEvent.h"

#include <cstdint>

namespace mbx {

// Turns a raw MIDI byte stream (e.g. the DIN input on the UART) into
// complete channel messages. Handles running status, ignores SysEx and
// system messages, and lets real-time bytes interleave without breaking
// the message in progress.
class MidiParser {
public:
    // Feed one byte. Returns true when `out` holds a complete message.
    bool feed(std::uint8_t byte, MidiEvent& out);
    void reset();

private:
    std::uint8_t runningStatus_ = 0;
    std::uint8_t data_[2] = {0, 0};
    int count_ = 0;
    bool inSysEx_ = false;
};

} // namespace mbx
