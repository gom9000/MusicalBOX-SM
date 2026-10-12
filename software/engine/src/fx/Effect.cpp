#include "fx/Effect.h"

#include "fx/Effects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace mbx {

using nlohmann::json;
using Kind = ParamSpec::Kind;

namespace {

constexpr int kGlideBlock = 32;      // while a parameter glides, it is updated every 32 samples
constexpr double kGlideSeconds = 0.03;

ParamSpec num(const char* name, float def, float lo, float hi, bool log = false, bool live = true) {
    return {name, Kind::Number, def, lo, hi, {}, live, log};
}
ParamSpec flag(const char* name, bool def, bool live) {
    return {name, Kind::Flag, def ? 1.0f : 0.0f, 0.0f, 1.0f, {}, live, false};
}
ParamSpec choice(const char* name, int def, std::vector<const char*> options, bool live) {
    const float last = static_cast<float>(options.size() - 1);
    return {name, Kind::Choice, static_cast<float>(def), 0.0f, last, std::move(options), live, false};
}

template <typename T>
std::unique_ptr<Effect> make() {
    return std::make_unique<T>();
}

void adjustChorus(std::vector<float>& v, const json& params) {
    if (v[0] < 0.5f) return;  // ce2: the defaults as written
    if (!params.contains("rate_hz")) v[1] = 0.6f;
    if (!params.contains("mix")) v[3] = 0.7f;
}

const std::vector<EffectType>& types() {
    static const std::vector<EffectType> list = {
        {"tone",
         {num("low_cut_hz", 0, 0, 1000), num("bass_db", 0, -18, 18), num("bass_hz", 120, 30, 500, true),
          num("mid_db", 0, -18, 18), num("mid_hz", 1000, 200, 5000, true), num("treble_db", 0, -18, 18),
          num("treble_hz", 3500, 1000, 12000, true), num("high_cut_hz", 0, 0, 20000), num("gain_db", 0, -24, 12)},
         make<Tone>},
        {"echo",
         {num("time_ms", 350, 20, 2000), num("feedback", 0.35f, 0, 0.95f), num("mix", 0.3f, 0, 1),
          num("tone_hz", 2800, 500, 12000, true), num("low_cut_hz", 120, 20, 1000, true), num("wow", 0.3f, 0, 1),
          num("drive", 0.3f, 0, 1), flag("ping_pong", false, false)},
         make<TapeEcho>},
        {"chorus",
         {choice("mode", 0, {"ce2", "ensemble"}, false), num("rate_hz", 0.8f, 0.05f, 8, true), num("depth", 0.5f, 0, 1),
          num("mix", 0.5f, 0, 1)},
         make<Chorus>, adjustChorus},
        {"phaser",
         {num("rate_hz", 0.5f, 0.05f, 5, true), num("depth", 1, 0, 1), num("feedback", 0.2f, 0, 0.8f),
          num("mix", 0.5f, 0, 1), num("stages", 4, 2, 12, false, false), flag("stereo", false, true)},
         make<Phaser>},
        {"leslie",
         {choice("speed", 0, {"slow", "fast"}, true), num("depth", 1, 0, 2), num("spread", 0.8f, 0, 1),
          num("drive", 0, 0, 1), num("mix", 1, 0, 1), num("cabinet", 1, 0, 2), num("top_hz", 2800, 1000, 20000, true)},
         make<Leslie>},
        {"tremolo",
         {num("rate_hz", 5, 0.1f, 20, true), num("depth", 0.5f, 0, 1),
          choice("shape", 0, {"sine", "triangle", "square"}, false), flag("stereo", false, true)},
         make<Tremolo>},
        {"fuzz",
         {num("drive", 0.6f, 0, 1), num("octave", 0.5f, 0, 1), num("bias", 0.2f, 0, 1),
          num("tone_hz", 2500, 500, 10000, true), num("low_cut_hz", 150, 20, 1000, true), num("blend", 1, 0, 1),
          num("level_db", 0, -24, 12)},
         make<Fuzz>},
        {"width", {num("amount", 0.5f, 0, 1), num("time_ms", 12, 3, 30), num("low_cut_hz", 250, 50, 2000, true)},
         make<Width>},
        {"room",
         {num("level", 0.5f, 0, 1.5f), num("time_s", 0.7f, 0.2f, 4), num("damp_hz", 3000, 1000, 16000, true),
          num("predelay_ms", 5, 0, 80), num("low_cut_hz", 250, 20, 1000, true)},
         make<Room>},
        {"motor", {choice("state", 0, {"on", "off"}, true), num("stop_s", 3, 0.1f, 30), num("start_s", 1, 0.1f, 30)},
         make<Motor>},
    };
    return list;
}

std::string format(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace

int EffectType::index(const std::string& param) const {
    for (std::size_t i = 0; i < params.size(); ++i)
        if (param == params[i].name) return static_cast<int>(i);
    return -1;
}

const EffectType* findEffectType(const std::string& name) {
    for (const auto& t : types())
        if (name == t.name) return &t;
    return nullptr;
}

std::string effectTypeList() {
    std::string s;
    for (const auto& t : types()) s += std::string(s.empty() ? "" : ", ") + t.name;
    return s;
}

std::string formatValue(const ParamSpec& spec, float v) {
    if (spec.kind == Kind::Flag) return v >= 0.5f ? "true" : "false";
    if (spec.kind == Kind::Choice) return std::string("\"") + spec.options[static_cast<std::size_t>(v + 0.5f)] + "\"";
    return format(v);
}

bool readValue(const ParamSpec& spec, const json& v, const std::string& where, std::vector<std::string>& warnings,
               float& out) {
    switch (spec.kind) {
    case Kind::Number: {
        if (!v.is_number()) return false;
        const float x = v.get<float>();
        if (x < spec.lo || x > spec.hi) {
            out = std::clamp(x, spec.lo, spec.hi);
            warnings.push_back(where + ": \"" + spec.name + "\" " + format(x) + " is outside " + format(spec.lo) +
                               ".." + format(spec.hi) + " - " + format(out) + " used");
        } else {
            out = x;
        }
        return true;
    }
    case Kind::Flag:
        if (!v.is_boolean()) return false;
        out = v.get<bool>() ? 1.0f : 0.0f;
        return true;
    case Kind::Choice:
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            for (std::size_t i = 0; i < spec.options.size(); ++i)
                if (s == spec.options[i]) {
                    out = static_cast<float>(i);
                    return true;
                }
        }
        return false;
    }
    return false;
}

std::vector<float> readParams(const EffectType& type, const json& paramsIn, const std::string& where,
                              std::vector<std::string>& warnings) {
    static const json empty = json::object();
    const json& params = paramsIn.is_object() ? paramsIn : empty;
    std::vector<float> values;
    for (const ParamSpec& spec : type.params) {
        float v = spec.def;
        if (params.contains(spec.name)) {
            float read = 0.0f;
            if (readValue(spec, params[spec.name], where, warnings, read)) {
                v = read;
            } else if (spec.kind == Kind::Number) {
                warnings.push_back(where + ": \"" + spec.name + "\" must be a number - default " + format(spec.def) +
                                   " used");
            } else if (spec.kind == Kind::Flag) {
                warnings.push_back(where + ": \"" + spec.name + "\" must be true or false");
            } else {
                std::string list;
                for (const char* o : spec.options) list += std::string(list.empty() ? "" : ", ") + "\"" + o + "\"";
                warnings.push_back(where + ": \"" + spec.name + "\" must be one of " + list + " - \"" +
                                   spec.options[static_cast<std::size_t>(spec.def)] + "\" used");
            }
        }
        values.push_back(v);
    }
    if (type.adjust) type.adjust(values, params);
    for (auto it = params.begin(); it != params.end(); ++it) {
        const std::string& key = it.key();
        if (type.index(key) >= 0 || key.rfind('_', 0) == 0) continue;
        if (key == "switch" || key == "bypass" || key == "cc")
            warnings.push_back(where + ": \"" + key + "\" goes next to \"params\", not inside it - ignored");
        else
            warnings.push_back(where + ": unknown parameter \"" + key + "\" ignored");
    }
    return values;
}

std::unique_ptr<Effect> createEffect(const std::string& type, const json& params, const std::string& where,
                                     std::vector<std::string>& warnings) {
    const EffectType* t = findEffectType(type);
    if (!t) {
        warnings.push_back(where + ": unknown effect type \"" + type + "\" (known: " + effectTypeList() +
                           ") - ignored");
        return nullptr;
    }
    auto effect = t->create();
    effect->init(*t, readParams(*t, params, where + " (" + type + ")", warnings));
    return effect;
}

// --- Effect ------------------------------------------------------------------

void Effect::init(const EffectType& type, const std::vector<float>& values) {
    type_ = &type;
    count_ = static_cast<int>(std::min<std::size_t>(values.size(), kMaxParams));
    for (int i = 0; i < count_; ++i) target_[i] = current_[i] = values[static_cast<std::size_t>(i)];
}

void Effect::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    glideSteps_ = std::max(1, static_cast<int>(std::lround(kGlideSeconds * sampleRate / kGlideBlock)));
    for (int i = 0; i < count_; ++i) {
        current_[i] = target_[i];
        stepsLeft_[i] = 0;
    }
    moving_ = false;
    allocate(sampleRate);
    update(true);
    reset();
    prepared_ = true;
}

void Effect::setParam(int index, float value, bool immediate) {
    if (index < 0 || index >= count_) return;
    const ParamSpec& spec = type_->params[static_cast<std::size_t>(index)];
    if (!spec.live) return;
    value = std::clamp(value, spec.lo, spec.hi);
    target_[index] = value;
    if (immediate || spec.kind != Kind::Number || !prepared_) {
        stepsLeft_[index] = 0;
        if (current_[index] == value && !immediate) return;
        current_[index] = value;
        if (prepared_) update(immediate);
    } else if (current_[index] != value) {
        // A straight glide from where it is now, 30 ms long.
        stepsLeft_[index] = glideSteps_;
        step_[index] = (value - current_[index]) / static_cast<float>(glideSteps_);
        moving_ = true;
    } else {
        stepsLeft_[index] = 0;
    }
}

void Effect::glide() {
    moving_ = false;
    for (int i = 0; i < count_; ++i) {
        if (stepsLeft_[i] == 0) continue;
        if (--stepsLeft_[i] == 0) {
            current_[i] = target_[i];  // exactly
        } else {
            current_[i] += step_[i];
            moving_ = true;
        }
    }
}

void Effect::run(float* left, float* right, int frames) {
    if (!moving_) {
        process(left, right, frames);
        return;
    }
    for (int done = 0; done < frames;) {
        const int n = std::min(kGlideBlock, frames - done);
        if (moving_) {
            glide();
            update(false);
        }
        process(left + done, right + done, n);
        done += n;
    }
}

} // namespace mbx
