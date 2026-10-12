#pragma once

#include "fx/FxChange.h"
#include "midi/MidiEvent.h"

#include <array>
#include <cstdint>

namespace mbx {

struct Program;
class FxChain;
constexpr int kEventPresetCount = 128;  // = kPresetCount (sampler/Program.h)

// Everything that reaches the audio thread travels as one of these,
// through the engine's lock-free queues.
struct EngineEvent {
    enum class Type : std::uint8_t {
        Midi,          // midi
        MasterGainDb,  // value = dB
        AllSoundOff,   // fade every voice out now
        SelectPreset,  // slot = 1..128
        FxSwitch,      // slot = -1 toggle, 0 position A, 1 position B
        SwapPresets,   // presets = new table, slot = generation (see Engine::appliedGeneration)
        Fx,            // chain, fx: a value for an effect that keeps playing; slot 1 = programmed value
        FxCurrent,     // chain, fx: a live control, applied only if that chain is playing
    };

    Type type = Type::Midi;
    MidiEvent midi{};
    float value = 0.0f;
    int slot = 0;
    const std::array<const Program*, kEventPresetCount>* presets = nullptr;
    FxChain* chain = nullptr;
    FxChange fx{};

    static EngineEvent fromMidi(const MidiEvent& m) {
        EngineEvent e;
        e.type = Type::Midi;
        e.midi = m;
        return e;
    }
    static EngineEvent masterGainDb(float db) {
        EngineEvent e;
        e.type = Type::MasterGainDb;
        e.value = db;
        return e;
    }
    static EngineEvent allSoundOff() {
        EngineEvent e;
        e.type = Type::AllSoundOff;
        return e;
    }
    static EngineEvent fxSwitch(int position = -1) {
        EngineEvent e;
        e.type = Type::FxSwitch;
        e.slot = position;
        return e;
    }
    // A new preset table (programs edited or reloaded). The program playing is
    // kept if the new table has it, and keeps its notes; its effects cross-fade.
    static EngineEvent swapPresets(const std::array<const Program*, kEventPresetCount>* table, int generation) {
        EngineEvent e;
        e.type = Type::SwapPresets;
        e.presets = table;
        e.slot = generation;
        return e;
    }
    // A new programmed value for a chain (web editor): the chain is shared by
    // the old and the new preset table, so it keeps playing.
    static EngineEvent fxDefine(FxChain* chain, const FxChange& change) {
        EngineEvent e;
        e.type = Type::Fx;
        e.chain = chain;
        e.fx = change;
        e.slot = 1;
        return e;
    }
    // A control moved by hand, for the program playing (its chain; ignored if
    // another program was selected in the meantime).
    static EngineEvent fxControl(const FxChain* chain, const FxChange& change) {
        EngineEvent e;
        e.type = Type::FxCurrent;
        e.chain = const_cast<FxChain*>(chain);
        e.fx = change;
        return e;
    }
    static EngineEvent selectPreset(int slot) {
        EngineEvent e;
        e.type = Type::SelectPreset;
        e.slot = slot;
        return e;
    }
};

} // namespace mbx
