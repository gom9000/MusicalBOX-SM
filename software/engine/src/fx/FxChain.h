#pragma once

#include "core/Constants.h"
#include "fx/Effect.h"
#include "fx/FxChange.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace mbx {

struct FxDef;
class Motor;

// The effects of one program, in the order written in programs.json.
//
// Build and prepare outside the audio thread; everything else in the audio
// thread. A live change (switch, controller, a value edited in the web
// editor) goes to the effect that is already playing: tails, phases and
// rotors go on. Only a different structure (other effects, order, a
// parameter that is not live) needs a new chain.
class FxChain {
public:
    // Effects with "bypass": true and unknown types are left out (with a warning).
    static std::shared_ptr<FxChain> build(const std::vector<FxDef>& defs, const std::string& where,
                                          std::vector<std::string>& warnings);

    void prepare(double sampleRate);
    void reset();     // clear tails
    void restore();   // every parameter back to its programmed value (program selected)
    void process(float* left, float* right, int frames);

    // The program's switch button: every effect with a "switch" goes to its
    // first (A) or second (B) value. immediate = no transition.
    void setSwitch(bool positionB, bool immediate);
    bool hasSwitch() const { return hasSwitch_; }

    // define = a new programmed value (web editor); otherwise a live control.
    void apply(const FxChange& change, bool define);
    // MIDI control change: the parameters mapped with "cc".
    void midiControl(int cc, int value);

    // The program's motor ("motor" in fx), nullptr if none: the engine reads
    // its speed for the voices.
    Motor* motor() const { return motor_; }

    std::size_t size() const { return slots_.size(); }
    // Position in the chain of the effect written as number `fx` (1-based) in
    // programs.json; -1 if it is not in the chain (bypassed, unknown).
    int slotOf(int fx) const;
    const EffectType& typeAt(int slot) const { return slots_[static_cast<std::size_t>(slot)].fx->type(); }
    const Effect& effectAt(int slot) const { return *slots_[static_cast<std::size_t>(slot)].fx; }  // diagnostics
    bool prepared() const { return prepared_; }

    // Outside the audio thread. A chain with the same structure can take the
    // values of `newer` through changes instead of being replaced.
    bool sameStructure(const FxChain& newer) const;
    std::vector<FxChange> changesTo(const FxChain& newer) const;
    void adoptValues(const FxChain& newer);  // the description, after the changes were sent

private:
    enum class Switch { None, Live, Twin, Bypass };
    struct Slot {
        std::unique_ptr<Effect> fx;
        std::unique_ptr<Effect> twin;  // second instance: a switch on a parameter that is not live
        Switch sw = Switch::None;
        int swParam = -1;
        float swA = 0.0f, swB = 0.0f;
        bool bypassA = false, bypassB = true;
        std::array<float, Effect::kMaxParams> programmed{};
        // Audio-thread state.
        bool twinPlaying = false;  // the twin is the one sounding
        int fade = 0;              // twin cross-fade, samples left
        bool bypassed = false;
        float wet = 1.0f;          // 1 = effect, 0 = dry
    };
    // What the chain was built from (outside the audio thread only).
    struct Description {
        std::string signature;     // type, non-live values, switch, controllers
        std::vector<float> values;
        float swA = 0.0f, swB = 0.0f;
    };
    struct Control {
        int cc, slot, param;  // param FxChange::kBypass for bypass
        float lo, hi;
        bool log;
        ParamSpec::Kind kind;
        int options;
    };

    void runEffect(Slot& s, float* left, float* right, int frames);
    void processSlot(Slot& s, float* left, float* right, int frames);
    void setBypass(Slot& s, bool bypassed, bool immediate);

    std::vector<Slot> slots_;
    std::vector<Description> desc_;
    std::vector<Control> controls_;
    std::vector<int> defIndex_;  // per slot: number of the effect in programs.json (1-based)
    bool hasSwitch_ = false;
    bool switchB_ = false;
    bool prepared_ = false;
    int fadeTotal_ = 1;
    Motor* motor_ = nullptr;
    std::array<float, kMaxBlockFrames> dryL_{}, dryR_{}, inL_{}, inR_{}, outL_{}, outR_{};
};

} // namespace mbx
