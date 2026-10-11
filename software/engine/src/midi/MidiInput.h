#pragma once

namespace mbx {

// A source of MIDI events for the engine. Each implementation pushes the
// events it receives into its own Engine source queue.
// Implementations: RtMidiInput (USB), UartMidiInput (DIN).
class MidiInput {
public:
    virtual ~MidiInput() = default;
    // Opens what is available now; called again periodically to pick up
    // devices plugged in later. Never called from the audio thread.
    virtual void scan() = 0;
    virtual void close() = 0;
};

} // namespace mbx
