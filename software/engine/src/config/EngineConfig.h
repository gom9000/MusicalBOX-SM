#pragma once

#include "core/Constants.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mbx {

// System parameters from config/engine.json. Every field has a default, so
// the file is optional. Command-line options override these values.
struct EngineSettings {
    std::string audioDevice;          // part of the device name; empty = system default
    unsigned sampleRate = 48000;
    unsigned periodFrames = 128;
    unsigned periods = 3;
    float masterVolumeDb = -6.0f;     // at power on
    int polyphony = kDefaultPolyphony;
    int maxLayers = kDefaultMaxLayers;
    int ramBudgetPercent = 70;        // of physical RAM, for samples; 0 = no limit
    int midiChannel = 0;              // 1..16, 0 = omni
    std::string midiSerial = "/dev/serial0";  // DIN MIDI on the UART; empty = disabled
    std::string controlSocket = "/tmp/mbx-engine.sock";  // for mbx-control; empty = disabled
    std::string recordFolder = "/tmp";                    // recordings (r in the console, web editor)
    // Mute pins (XSMT) of the two PCM5102A DACs: the program's "output" A / B / AB.
    struct DacMute {
        bool enabled = true;
        int a = 16;          // BCM GPIO of DAC A's XSMT
        int b = 17;          // BCM GPIO of DAC B's XSMT
        std::string chip;    // "" = the Pi's own GPIO chip
    } dacMute;
};

// Reads engine.json if it exists (found = false otherwise, not an error).
// Invalid values keep their default and produce a warning; false only when
// the file exists but is not valid JSON.
bool loadEngineSettings(const std::filesystem::path& file, EngineSettings& out, bool& found,
                        std::vector<std::string>& warnings, std::string& error);

} // namespace mbx
