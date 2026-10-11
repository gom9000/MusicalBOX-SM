#pragma once

#include "midi/MidiInput.h"

#include <memory>
#include <string>
#include <vector>

class RtMidiIn;

namespace mbx {

class Engine;

// USB (and any other system) MIDI inputs through RtMidi. Every port found is
// opened, except the "Midi Through" loopback; ports that appear later are
// opened on the next scan(), ports that disappear are closed.
class RtMidiInput : public MidiInput {
public:
    explicit RtMidiInput(Engine& engine);
    ~RtMidiInput() override;

    void scan() override;
    void close() override;

    // False after a scan that could not reach the MIDI system at all
    // (the caller then rescans less often).
    bool systemAvailable() const { return systemAvailable_; }

    static void listPorts();

private:
    struct Port;
    static void onMessage(double timestamp, std::vector<unsigned char>* message, void* user);

    Engine& engine_;
    std::vector<std::unique_ptr<Port>> ports_;
    bool systemAvailable_ = true;
};

} // namespace mbx
