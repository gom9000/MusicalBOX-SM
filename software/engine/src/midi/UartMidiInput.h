#pragma once

#include "midi/MidiInput.h"

#include <atomic>
#include <string>
#include <thread>

namespace mbx {

class Engine;

// DIN MIDI on the Raspberry Pi UART (/dev/serial0, an opto-isolated
// interface on GPIO15). The port is opened at 38400 baud: with
// "dtoverlay=midi-uart0" in config.txt the UART clock is scaled so that
// 38400 really is the MIDI rate, 31250.
//
// A reader thread waits on the port, feeds the bytes to a MidiParser
// (running status, real-time bytes and SysEx handled there) and posts the
// channel messages to the engine. If the port goes away or fails, the
// thread stops and the next scan() tries to open it again.
class UartMidiInput : public MidiInput {
public:
    // device: path of the serial port; empty = disabled.
    UartMidiInput(Engine& engine, std::string device);
    ~UartMidiInput() override;

    void scan() override;
    void close() override;

    bool enabled() const { return !device_.empty(); }
    bool isOpen() const { return fd_ >= 0; }
    const std::string& device() const { return device_; }
    // Bytes received since start, real-time bytes included.
    unsigned long bytesReceived() const { return bytes_.load(std::memory_order_relaxed); }

    // For --list-midi: what is at `device`.
    static void describe(const std::string& device);

private:
    bool open();
    void run();

    Engine& engine_;
    std::string device_;
    int fd_ = -1;
    int source_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<unsigned long> bytes_{0};
    std::string lastProblem_;  // to log each problem once, not at every scan
};

} // namespace mbx
