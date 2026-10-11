#include "fx/Effect.h"

#include "fx/Effects.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace mbx {

using nlohmann::json;

namespace {
const json& emptyObject() {
    static const json empty = json::object();
    return empty;
}

std::string format(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
} // namespace

FxParams::FxParams(const json& params, std::string where, std::vector<std::string>& warnings)
    : params_(params.is_object() ? params : emptyObject()), where_(std::move(where)), warnings_(warnings) {}

FxParams::~FxParams() {
    for (auto it = params_.begin(); it != params_.end(); ++it) {
        const std::string& key = it.key();
        if (used_.count(key) || key.rfind('_', 0) == 0) continue;
        if (key == "switch" || key == "bypass")
            warnings_.push_back(where_ + ": \"" + key + "\" goes next to \"params\", not inside it - ignored");
        else
            warnings_.push_back(where_ + ": unknown parameter \"" + key + "\" ignored");
    }
}

float FxParams::number(const char* key, float fallback, float lo, float hi) {
    used_.insert(key);
    if (!params_.contains(key)) return fallback;
    const json& v = params_[key];
    if (!v.is_number()) {
        warnings_.push_back(where_ + ": \"" + key + "\" must be a number - default " + format(fallback) + " used");
        return fallback;
    }
    const float x = v.get<float>();
    if (x < lo || x > hi) {
        const float c = std::clamp(x, lo, hi);
        warnings_.push_back(where_ + ": \"" + key + "\" " + format(x) + " is outside " + format(lo) + ".." +
                            format(hi) + " - " + format(c) + " used");
        return c;
    }
    return x;
}

bool FxParams::flag(const char* key, bool fallback) {
    used_.insert(key);
    if (!params_.contains(key)) return fallback;
    if (!params_[key].is_boolean()) {
        warnings_.push_back(where_ + ": \"" + key + "\" must be true or false");
        return fallback;
    }
    return params_[key].get<bool>();
}

std::string FxParams::choice(const char* key, const std::string& fallback,
                             std::initializer_list<const char*> options) {
    used_.insert(key);
    if (!params_.contains(key)) return fallback;
    const json& v = params_[key];
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        for (const char* o : options)
            if (s == o) return s;
    }
    std::string list;
    for (const char* o : options) list += std::string(list.empty() ? "" : ", ") + "\"" + o + "\"";
    warnings_.push_back(where_ + ": \"" + key + "\" must be one of " + list + " - \"" + fallback + "\" used");
    return fallback;
}

std::unique_ptr<Effect> createEffect(const std::string& type, const json& params, const std::string& where,
                                     std::vector<std::string>& warnings) {
    static const char* const kTypes[] = {"tone", "echo", "chorus", "phaser", "leslie", "tremolo", "fuzz", "width", "room"};
    if (std::find(std::begin(kTypes), std::end(kTypes), type) == std::end(kTypes)) {
        warnings.push_back(where + ": unknown effect type \"" + type +
                           "\" (known: tone, echo, chorus, phaser, leslie, tremolo, fuzz, width, room) - ignored");
        return nullptr;
    }
    FxParams p(params, where + " (" + type + ")", warnings);
    if (type == "tone") return std::make_unique<Tone>(p);
    if (type == "echo") return std::make_unique<TapeEcho>(p);
    if (type == "chorus") return std::make_unique<Chorus>(p);
    if (type == "phaser") return std::make_unique<Phaser>(p);
    if (type == "leslie") return std::make_unique<Leslie>(p);
    if (type == "fuzz") return std::make_unique<Fuzz>(p);
    if (type == "width") return std::make_unique<Width>(p);
    if (type == "room") return std::make_unique<Room>(p);
    return std::make_unique<Tremolo>(p);
}

} // namespace mbx
