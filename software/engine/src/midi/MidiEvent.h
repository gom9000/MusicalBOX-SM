#pragma once

#include <cstdint>

namespace mbx {

// A complete channel-voice MIDI message (1 to 3 bytes).
struct MidiEvent {
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;

    std::uint8_t command() const { return status & 0xF0; }
    int channel() const { return (status & 0x0F) + 1; }  // 1..16
};

namespace midi {
constexpr std::uint8_t kNoteOff = 0x80;
constexpr std::uint8_t kNoteOn = 0x90;
constexpr std::uint8_t kPolyPressure = 0xA0;
constexpr std::uint8_t kControlChange = 0xB0;
constexpr std::uint8_t kProgramChange = 0xC0;
constexpr std::uint8_t kChannelPressure = 0xD0;
constexpr std::uint8_t kPitchBend = 0xE0;

constexpr std::uint8_t kCcSustain = 64;
constexpr std::uint8_t kCcAllSoundOff = 120;
constexpr std::uint8_t kCcAllNotesOff = 123;

// Number of data bytes that follow a status byte (channel messages only).
inline int dataLength(std::uint8_t status) {
    const std::uint8_t cmd = status & 0xF0;
    return (cmd == kProgramChange || cmd == kChannelPressure) ? 1 : 2;
}
} // namespace midi

} // namespace mbx
