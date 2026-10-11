#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mbx {

class Engine;

struct AudioOptions {
    std::string deviceName;      // substring of the device name; empty = default device
    std::uint32_t sampleRate = 48000;
    std::uint32_t periodFrames = 128;
    std::uint32_t periods = 3;
};

// Audio output through miniaudio (ALSA on the Pi, the native API on a PC).
// The device thread runs at real-time priority and calls Engine::render.
class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Opens the device and calls engine.prepare() with the actual sample rate.
    bool open(Engine& engine, const AudioOptions& options, std::string& error);
    bool start(std::string& error);
    void stop();

    std::string deviceName() const;
    std::uint32_t sampleRate() const;
    std::uint32_t periodFrames() const;

    // Audio thread load since the last call. 1.0 = all the available time.
    struct LoadStats {
        float maxLoad = 0.0f;      // worst single callback
        float averageLoad = 0.0f;  // over the whole interval
        std::uint64_t frames = 0;  // frames rendered in the interval
    };
    LoadStats takeLoadStats();
    std::uint64_t callbacks() const;

    static void listDevices();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mbx
