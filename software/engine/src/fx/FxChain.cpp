#include "fx/FxChain.h"

#include "fx/Effects.h"
#include "sampler/Program.h"

#include <algorithm>
#include <cmath>

namespace mbx {

using nlohmann::json;
using Kind = ParamSpec::Kind;

namespace {

std::string text(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

std::string valueText(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

} // namespace

std::shared_ptr<FxChain> FxChain::build(const std::vector<FxDef>& defs, const std::string& where,
                                        std::vector<std::string>& warnings) {
    auto chain = std::make_shared<FxChain>();
    int index = 0;
    for (const FxDef& def : defs) {
        ++index;
        if (def.bypass) continue;
        const std::string fxWhere = where + " fx " + std::to_string(index);
        const json params = def.paramsJson.empty() ? json::object() : json::parse(def.paramsJson);
        const EffectType* type = findEffectType(def.type);
        if (def.type == "motor" && chain->motor_) {
            warnings.push_back(fxWhere + " (motor): only one motor per program - ignored");
            continue;
        }

        Slot slot;
        Description desc;
        json paramsA = params;
        const std::string swWhere = fxWhere + " (" + def.type + ") switch";
        json a, b;
        int swIndex = -1;
        if (type && !def.switchParam.empty()) {
            a = json::parse(def.switchA);
            b = json::parse(def.switchB);
            if (def.switchParam == "bypass") {
                if (!a.is_boolean() || !b.is_boolean() || a == b) {
                    warnings.push_back(swWhere + ": [\"bypass\", A, B] needs true and false - switch ignored");
                } else {
                    slot.sw = Switch::Bypass;
                    slot.bypassA = a.get<bool>();
                    slot.bypassB = b.get<bool>();
                    slot.bypassed = slot.bypassA;
                    slot.wet = slot.bypassA ? 0.0f : 1.0f;
                }
            } else if ((swIndex = type->index(def.switchParam)) < 0) {
                warnings.push_back(swWhere + ": unknown parameter \"" + def.switchParam + "\" - switch ignored");
            } else {
                const std::string& param = def.switchParam;
                if (a == b) warnings.push_back(swWhere + ": the two values are the same");
                if (params.contains(param) && params[param] != a)
                    warnings.push_back(swWhere + ": \"" + param + "\" is " + text(params[param]) +
                                       " in params but the switch starts from " + text(a) + " - " + text(a) + " used");
                paramsA[param] = a;
                slot.sw = type->params[static_cast<std::size_t>(swIndex)].live ? Switch::Live : Switch::Twin;
                slot.swParam = swIndex;
            }
        }

        auto effect = createEffect(def.type, paramsA, fxWhere, warnings);
        if (!effect) continue;
        const EffectType& t = effect->type();
        for (std::size_t i = 0; i < t.params.size(); ++i) {
            slot.programmed[i] = effect->param(static_cast<int>(i));
            desc.values.push_back(slot.programmed[i]);
        }

        if (slot.sw == Switch::Live || slot.sw == Switch::Twin) {
            const ParamSpec& spec = t.params[static_cast<std::size_t>(swIndex)];
            std::vector<std::string> bWarnings;
            float vb = spec.def;
            if (!readValue(spec, b, fxWhere + " (" + def.type + ")", bWarnings, vb))
                bWarnings.push_back(fxWhere + " (" + def.type + "): \"" + spec.name + "\" " + text(b) +
                                    " is not a valid value");
            for (const auto& w : bWarnings) warnings.push_back(w + " (switch value B)");
            slot.swA = slot.programmed[static_cast<std::size_t>(swIndex)];
            slot.swB = vb;
            if (slot.sw == Switch::Twin) {
                json paramsB = params;
                paramsB[def.switchParam] = b;
                std::vector<std::string> ignored;  // reported above
                slot.twin = createEffect(def.type, paramsB, fxWhere, ignored);
            }
            desc.swA = slot.swA;
            desc.swB = slot.swB;
        }
        if (slot.sw != Switch::None) chain->hasSwitch_ = true;

        // Structure: what a value change cannot reach.
        desc.signature = def.type;
        for (std::size_t i = 0; i < t.params.size(); ++i)
            if (!t.params[i].live) desc.signature.append("|").append(valueText(desc.values[i]));
        desc.signature.append("|sw").append(std::to_string(static_cast<int>(slot.sw)));
        desc.signature.append(":").append(std::to_string(slot.swParam));
        if (slot.sw == Switch::Twin)
            desc.signature.append(":").append(valueText(slot.swA)).append(":").append(valueText(slot.swB));
        if (slot.sw == Switch::Bypass) desc.signature.append(slot.bypassA ? ":1" : ":0");

        // Controllers: "cc": { "mix": 21, "feedback": [22, 0.1, 0.6], "bypass": 23 }.
        const int slotIndex = static_cast<int>(chain->slots_.size());
        if (!def.ccJson.empty()) {
            const json cc = json::parse(def.ccJson);
            desc.signature.append("|cc").append(def.ccJson);
            const std::string ccWhere = fxWhere + " (" + def.type + ") cc";
            for (auto it = cc.begin(); it != cc.end(); ++it) {
                const std::string& name = it.key();
                const json& v = it.value();
                const json& number = v.is_array() && !v.empty() ? v[0] : v;
                if (!number.is_number_integer() || number.get<int>() < 0 || number.get<int>() > 119 ||
                    number.get<int>() == 64) {
                    warnings.push_back(ccWhere + ": \"" + name +
                                       "\" needs a controller number 0..119 (not 64, the sustain pedal) - ignored");
                    continue;
                }
                Control c{number.get<int>(), slotIndex, 0, 0.0f, 1.0f, false, Kind::Flag, 2};
                if (name == "bypass") {
                    c.param = FxChange::kBypass;
                } else {
                    const int pi = t.index(name);
                    if (pi < 0) {
                        warnings.push_back(ccWhere + ": unknown parameter \"" + name + "\" - ignored");
                        continue;
                    }
                    const ParamSpec& spec = t.params[static_cast<std::size_t>(pi)];
                    if (!spec.live) {
                        warnings.push_back(ccWhere + ": \"" + name + "\" cannot change while playing - ignored");
                        continue;
                    }
                    c.param = pi;
                    c.kind = spec.kind;
                    c.lo = spec.lo;
                    c.hi = spec.hi;
                    c.log = spec.log && spec.lo > 0.0f;
                    c.options = static_cast<int>(spec.options.size());
                    if (v.is_array() && v.size() == 3 && spec.kind == Kind::Number) {
                        float lo = c.lo, hi = c.hi;
                        if (readValue(spec, v[1], ccWhere, warnings, lo) && readValue(spec, v[2], ccWhere, warnings, hi)) {
                            c.lo = lo;
                            c.hi = hi;
                            c.log = c.log && lo > 0.0f && hi > 0.0f;
                        } else {
                            warnings.push_back(ccWhere + ": \"" + name + "\" range must be two numbers - full range used");
                        }
                    } else if (v.is_array() && v.size() != 1) {
                        warnings.push_back(ccWhere + ": \"" + name +
                                           "\" must be a controller number or [number, min, max] - full range used");
                    }
                }
                chain->controls_.push_back(c);
            }
        }

        if (def.type == "motor") chain->motor_ = static_cast<Motor*>(effect.get());
        slot.fx = std::move(effect);
        chain->slots_.push_back(std::move(slot));
        chain->desc_.push_back(std::move(desc));
        chain->defIndex_.push_back(index);
    }
    return chain;
}

void FxChain::prepare(double sampleRate) {
    for (auto& s : slots_) {
        s.fx->prepare(sampleRate);
        if (s.twin) s.twin->prepare(sampleRate);
    }
    fadeTotal_ = std::max(1, static_cast<int>(kFxFadeMs * 0.001 * sampleRate));
    prepared_ = true;
}

void FxChain::reset() {
    for (auto& s : slots_) {
        s.fx->reset();
        if (s.twin) s.twin->reset();
        s.fade = 0;
    }
}

void FxChain::restore() {
    for (auto& s : slots_) {
        const auto& params = s.fx->type().params;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (!params[i].live || (s.sw == Switch::Live && static_cast<int>(i) == s.swParam)) continue;
            s.fx->setParam(static_cast<int>(i), s.programmed[i], true);
            if (s.twin) s.twin->setParam(static_cast<int>(i), s.programmed[i], true);
        }
        if (s.sw != Switch::Bypass) setBypass(s, false, true);
    }
}

void FxChain::setBypass(Slot& s, bool bypassed, bool immediate) {
    s.bypassed = bypassed;
    if (immediate) {
        if (bypassed && s.wet > 0.0f) {
            s.fx->reset();
            if (s.twin) s.twin->reset();
        }
        s.wet = bypassed ? 0.0f : 1.0f;
    }
}

void FxChain::setSwitch(bool positionB, bool immediate) {
    switchB_ = positionB;
    for (auto& s : slots_) {
        switch (s.sw) {
        case Switch::None:
            break;
        case Switch::Live:
            s.fx->setParam(s.swParam, positionB ? s.swB : s.swA, immediate);
            break;
        case Switch::Bypass:
            setBypass(s, positionB ? s.bypassB : s.bypassA, immediate);
            break;
        case Switch::Twin:
            if (positionB == s.twinPlaying) break;
            s.twinPlaying = positionB;
            if (immediate) {
                s.fade = 0;
                (positionB ? s.fx : s.twin)->reset();
            } else {
                s.fade = fadeTotal_;
                (positionB ? s.twin : s.fx)->reset();  // the one coming in starts clean
            }
            break;
        }
    }
}

void FxChain::apply(const FxChange& c, bool define) {
    if (c.slot < 0 || c.slot >= static_cast<int>(slots_.size())) return;
    Slot& s = slots_[static_cast<std::size_t>(c.slot)];
    if (c.param == FxChange::kSwitchValues) {
        s.swA = c.value;
        s.swB = c.value2;
        if (s.sw == Switch::Live) s.fx->setParam(s.swParam, switchB_ ? s.swB : s.swA, false);
        return;
    }
    if (c.param == FxChange::kBypass) {
        if (!define) setBypass(s, c.value >= 0.5f, false);
        return;
    }
    if (c.param < 0 || c.param >= Effect::kMaxParams) return;
    if (define) {
        s.programmed[static_cast<std::size_t>(c.param)] = c.value;
        if (s.sw == Switch::Live && c.param == s.swParam) return;  // the switch values decide
    }
    s.fx->setParam(c.param, c.value, false);
    if (s.twin) s.twin->setParam(c.param, c.value, false);
}

void FxChain::midiControl(int cc, int value) {
    for (const Control& c : controls_) {
        if (c.cc != cc) continue;
        const float x = static_cast<float>(value) / 127.0f;
        float v;
        if (c.param == FxChange::kBypass || c.kind == Kind::Flag)
            v = value >= 64 ? 1.0f : 0.0f;
        else if (c.kind == Kind::Choice)
            v = static_cast<float>(std::min(c.options - 1, value * c.options / 128));
        else if (c.log)
            v = c.lo * std::pow(c.hi / c.lo, x);
        else
            v = c.lo + (c.hi - c.lo) * x;
        apply({c.slot, c.param, v, 0.0f}, false);
    }
}

void FxChain::runEffect(Slot& s, float* left, float* right, int frames) {
    if (!s.twin) {
        s.fx->run(left, right, frames);
        return;
    }
    Effect* in = s.twinPlaying ? s.twin.get() : s.fx.get();
    Effect* out = s.twinPlaying ? s.fx.get() : s.twin.get();
    if (s.fade == 0) {
        in->run(left, right, frames);
        return;
    }
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, kMaxBlockFrames);
        float* l = left + done;
        float* r = right + done;
        std::copy_n(l, n, inL_.data());
        std::copy_n(r, n, inR_.data());
        std::copy_n(l, n, outL_.data());
        std::copy_n(r, n, outR_.data());
        in->run(inL_.data(), inR_.data(), n);
        out->run(outL_.data(), outR_.data(), n);
        const float step = 1.0f / static_cast<float>(fadeTotal_);
        float g = 1.0f - static_cast<float>(s.fade) * step;  // weight of the new one
        for (int i = 0; i < n; ++i) {
            g = std::min(1.0f, g + step);
            l[i] = g * inL_[i] + (1.0f - g) * outL_[i];
            r[i] = g * inR_[i] + (1.0f - g) * outR_[i];
        }
        s.fade = std::max(0, s.fade - n);
        done += n;
        if (s.fade == 0) {
            out->reset();
            if (done < frames) in->run(left + done, right + done, frames - done);
            break;
        }
    }
}

void FxChain::processSlot(Slot& s, float* left, float* right, int frames) {
    if (s.bypassed && s.wet == 0.0f) return;  // dry
    if (!s.bypassed && s.wet == 1.0f) {
        runEffect(s, left, right, frames);
        return;
    }
    // Bypass moving: effect and dry sound cross-faded.
    const float step = 1.0f / static_cast<float>(fadeTotal_);
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, kMaxBlockFrames);
        float* l = left + done;
        float* r = right + done;
        std::copy_n(l, n, dryL_.data());
        std::copy_n(r, n, dryR_.data());
        runEffect(s, l, r, n);
        float g = s.wet;
        for (int i = 0; i < n; ++i) {
            g = s.bypassed ? std::max(0.0f, g - step) : std::min(1.0f, g + step);
            l[i] = dryL_[i] + g * (l[i] - dryL_[i]);
            r[i] = dryR_[i] + g * (r[i] - dryR_[i]);
        }
        s.wet = g;
        done += n;
        if (s.bypassed && g == 0.0f) {
            // Dry from here on; the effect starts clean when it comes back.
            s.fx->reset();
            if (s.twin) s.twin->reset();
            break;
        }
        if (!s.bypassed && g == 1.0f) {
            if (done < frames) runEffect(s, left + done, right + done, frames - done);
            break;
        }
    }
}

void FxChain::process(float* left, float* right, int frames) {
    for (auto& s : slots_) processSlot(s, left, right, frames);
}

int FxChain::slotOf(int fx) const {
    for (std::size_t k = 0; k < defIndex_.size(); ++k)
        if (defIndex_[k] == fx) return static_cast<int>(k);
    return -1;
}

bool FxChain::sameStructure(const FxChain& newer) const {
    if (desc_.size() != newer.desc_.size()) return false;
    for (std::size_t k = 0; k < desc_.size(); ++k)
        if (desc_[k].signature != newer.desc_[k].signature) return false;
    return true;
}

std::vector<FxChange> FxChain::changesTo(const FxChain& newer) const {
    std::vector<FxChange> changes;
    for (std::size_t k = 0; k < desc_.size() && k < newer.desc_.size(); ++k) {
        const Description& d = desc_[k];
        const Description& n = newer.desc_[k];
        const Slot& s = slots_[k];
        const auto& params = s.fx->type().params;
        for (std::size_t i = 0; i < params.size() && i < d.values.size() && i < n.values.size(); ++i) {
            if (!params[i].live || d.values[i] == n.values[i]) continue;
            if (s.sw == Switch::Live && static_cast<int>(i) == s.swParam) continue;
            changes.push_back({static_cast<int>(k), static_cast<int>(i), n.values[i], 0.0f});
        }
        if (s.sw == Switch::Live && (d.swA != n.swA || d.swB != n.swB))
            changes.push_back({static_cast<int>(k), FxChange::kSwitchValues, n.swA, n.swB});
    }
    return changes;
}

void FxChain::adoptValues(const FxChain& newer) { desc_ = newer.desc_; }

} // namespace mbx
