#pragma once

#include "core/EngineEvent.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace mbx {

class Instrument;
class FxChain;

struct Adsr {
    float attackMs = 2.0f;
    float decayMs = 0.0f;
    float sustain = 1.0f;   // 0..1
    float releaseMs = 150.0f;
};

enum class VelocityCurve {
    Linear,  // gain = x
    Exp,     // 40 dB range: gain = 10^(-(1 - x) * 40 / 20)
};

// How incoming velocity is used.
//  - disabled: every note plays as velocity 127 at full gain;
//  - enabled: velocity is quantised to `levels` steps; the step picks the
//    sample layer and, through the curve, the gain.
struct VelocitySettings {
    bool enabled = false;
    int levels = 127;  // 1..127
    VelocityCurve curve = VelocityCurve::Linear;

    struct Mapped {
        int lookupVelocity;  // 1..127, used to choose the zone
        float gain;          // 0..1
    };
    Mapped map(int velocity) const;
};

// Which DAC(s) the program plays on (mute pins of the DACs, app/OutputRouter).
enum class Output { A, B, AB };

const char* outputName(Output output);

// An effect as written in programs.json; FxChain builds the real effect from it.
struct FxDef {
    std::string type;
    std::string paramsJson;  // the "params" object, serialised
    bool bypass = false;
    // "switch": [param, A, B] - the program's switch button moves `param`
    // between A (at program selection) and B. Values serialised as JSON.
    std::string switchParam;
    std::string switchA, switchB;
};

// One instrument inside a program, with its own rules. A program has at most
// "max_layers" of them (engine.json, default 3), stacked or split.
struct Layer {
    const Instrument* instrument = nullptr;
    int keyLo = 0;
    int keyHi = 127;
    int transpose = 0;      // semitones
    float gain = 1.0f;      // linear
    Adsr adsr;
    VelocitySettings velocity;
    bool loop = false;      // "organ mode": hold notes by looping the smpl loop
};

// The finished sound selected from a preset button.
struct Program {
    std::string id;
    std::string name;
    Output output = Output::A;
    float gain = 1.0f;      // linear
    std::vector<Layer> layers;
    std::vector<FxDef> fx;
    // Built from `fx` when the program is loaded. The chain has state (delay
    // lines...), so the engine is its only user once the audio runs.
    std::shared_ptr<FxChain> fxChain;
};

// Presets 1..128, the whole MIDI Program Change range (PC 0..127) -> program
// (nullptr = not defined). presets.json defines as many as needed.
constexpr int kPresetCount = 128;
using PresetTable = std::array<const Program*, kPresetCount>;
static_assert(kPresetCount == kEventPresetCount, "EngineEvent::presets uses the same array type");

float dbToGain(float db);

} // namespace mbx
