#pragma once

#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mbx {

// A stereo effect with fixed parameters (set once from programs.json).
//
// Threading: the constructor and prepare() allocate and run before the audio
// starts; reset() and process() run in the audio thread and never allocate.
class Effect {
public:
    virtual ~Effect() = default;
    virtual void prepare(double sampleRate) = 0;
    virtual void reset() = 0;                               // clear tails and state
    virtual void process(float* left, float* right, int frames) = 0;

    // Switch between two values of one parameter ("switch" in programs.json).
    // configureSwitch() runs when the program is loaded; an effect that can
    // change that parameter by itself (e.g. the Leslie speed, with its real
    // acceleration) returns true. Otherwise FxChain uses two instances.
    virtual bool configureSwitch(const std::string& /*param*/, const nlohmann::json& /*a*/,
                                 const nlohmann::json& /*b*/) {
        return false;
    }
    // Audio thread. immediate = no transition (program just selected).
    virtual void setSwitch(bool /*positionB*/, bool /*immediate*/) {}
};

// Reads the "params" object of an effect: missing values take the default,
// values out of range are clamped, unknown keys and wrong types are reported.
class FxParams {
public:
    FxParams(const nlohmann::json& params, std::string where, std::vector<std::string>& warnings);
    ~FxParams();  // reports the keys nobody asked for

    float number(const char* key, float fallback, float lo, float hi);
    bool flag(const char* key, bool fallback);
    std::string choice(const char* key, const std::string& fallback, std::initializer_list<const char*> options);

private:
    const nlohmann::json& params_;
    std::string where_;
    std::vector<std::string>& warnings_;
    std::set<std::string> used_;
};

// Builds one effect from its type name; nullptr (with a warning) if the type is unknown.
std::unique_ptr<Effect> createEffect(const std::string& type, const nlohmann::json& params, const std::string& where,
                                     std::vector<std::string>& warnings);

} // namespace mbx
