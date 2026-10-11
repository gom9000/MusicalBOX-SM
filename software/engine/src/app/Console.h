#pragma once

#include "sampler/Program.h"

#include <functional>
#include <thread>

namespace mbx {

class Engine;

// Commands typed in the terminal while the engine plays (tests, rehearsal
// with a PC). Started only when stdin is a terminal.
//   1..128     select preset
//   v -12      master volume in dB
//   x          all sound off
//   r          start / stop recording
//   s          status
//   q          quit
class Console {
public:
    // Program of a preset button, to report empty slots (the table can be
    // replaced while the engine runs); empty function in test mode.
    using PresetLookup = std::function<const Program*(int slot)>;
    Console(Engine& engine, PresetLookup presets, std::function<void()> onStatus, std::function<void()> onQuit,
            std::function<void()> onRecord = {});
    ~Console();

    // Returns false when stdin is not interactive (e.g. under systemd).
    bool start();

private:
    void run();

    Engine& engine_;
    PresetLookup presets_;
    std::function<void()> onStatus_;
    std::function<void()> onQuit_;
    std::function<void()> onRecord_;
    int source_ = -1;
    std::thread thread_;
};

} // namespace mbx
