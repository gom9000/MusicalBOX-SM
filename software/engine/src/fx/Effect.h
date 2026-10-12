#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mbx {

// One parameter of an effect type. Every value is held as a float: numbers
// as they are, flags as 0/1, choices as the index of the option.
struct ParamSpec {
    enum class Kind : std::uint8_t { Number, Flag, Choice };
    const char* name;
    Kind kind;
    float def, lo, hi;                 // Choice: index of the default option, 0, count - 1
    std::vector<const char*> options;  // Choice only
    bool live;                         // can change while playing, keeping the effect's state
    bool log;                          // controllers move it on a logarithmic scale (frequencies)
};

class Effect;

// An effect type: its parameters and how to make one.
struct EffectType {
    const char* name;
    std::vector<ParamSpec> params;
    std::unique_ptr<Effect> (*create)();
    // Defaults that depend on another parameter (chorus: rate and mix by mode).
    void (*adjust)(std::vector<float>& values, const nlohmann::json& params) = nullptr;

    int index(const std::string& param) const;  // -1 if unknown
};

const EffectType* findEffectType(const std::string& name);
std::string effectTypeList();  // "tone, echo, ..." for messages

// A stereo effect whose parameters can change while it plays.
//
// Threading: init() and prepare() allocate and run outside the audio thread
// (prepare() sizes the buffers for the whole range of every parameter);
// reset(), run() and setParam() run in the audio thread and never allocate.
// A live parameter changes without clearing tails or phases; numbers glide
// to the new value in 30 ms, so a control moved by hand is smooth.
class Effect {
public:
    static constexpr int kMaxParams = 12;

    virtual ~Effect() = default;

    void init(const EffectType& type, const std::vector<float>& values);
    void prepare(double sampleRate);
    virtual void reset() = 0;  // clear tails and state, keep the parameters

    void run(float* left, float* right, int frames);
    // Audio thread. Non-live parameters are ignored (they need a new instance).
    void setParam(int index, float value, bool immediate);
    float param(int index) const { return current_[static_cast<std::size_t>(index)]; }
    const EffectType& type() const { return *type_; }
    bool prepared() const { return prepared_; }

protected:
    virtual void allocate(double sampleRate) = 0;       // buffers for every possible value
    virtual void update(bool immediate) = 0;            // derived values from param(); no allocation, no reset
    virtual void process(float* left, float* right, int frames) = 0;

    bool flag(int index) const { return param(index) >= 0.5f; }
    int choice(int index) const { return static_cast<int>(param(index) + 0.5f); }

    double sampleRate_ = 48000.0;

private:
    void glide();

    const EffectType* type_ = nullptr;
    int count_ = 0;
    std::array<float, kMaxParams> target_{}, current_{}, step_{};
    std::array<int, kMaxParams> stepsLeft_{};
    bool moving_ = false;
    bool prepared_ = false;
    int glideSteps_ = 1;
};

// Reads the "params" object of an effect against its type: missing values take
// the default, values out of range are clamped, unknown keys and wrong types
// are reported.
std::vector<float> readParams(const EffectType& type, const nlohmann::json& params, const std::string& where,
                              std::vector<std::string>& warnings);
// One value, for a switch or a controller range; false (with a warning) if unusable.
bool readValue(const ParamSpec& spec, const nlohmann::json& v, const std::string& where,
               std::vector<std::string>& warnings, float& out);
std::string formatValue(const ParamSpec& spec, float v);

// Builds one effect from its type name; nullptr (with a warning) if the type is unknown.
std::unique_ptr<Effect> createEffect(const std::string& type, const nlohmann::json& params, const std::string& where,
                                     std::vector<std::string>& warnings);

} // namespace mbx
