#include "fx/FxChain.h"

#include "core/Constants.h"
#include "fx/Effects.h"
#include "sampler/Program.h"

#include <algorithm>
#include <array>

namespace mbx {

using nlohmann::json;

namespace {

// Generic switch: two versions of the effect (either may be "no effect", for
// the bypass switch) and a short crossfade between them. Only the selected
// one runs, except during the crossfade.
class SwitchedEffect : public Effect {
public:
    SwitchedEffect(std::unique_ptr<Effect> a, std::unique_ptr<Effect> b) : a_(std::move(a)), b_(std::move(b)) {}

    void prepare(double sampleRate) override {
        if (a_) a_->prepare(sampleRate);
        if (b_) b_->prepare(sampleRate);
        fadeTotal_ = std::max(1, static_cast<int>(kFxFadeMs * 0.001 * sampleRate));
        reset();
    }
    void reset() override {
        if (a_) a_->reset();
        if (b_) b_->reset();
        fadeRemaining_ = 0;
        positionB_ = false;
    }
    void setSwitch(bool positionB, bool immediate) override {
        if (positionB == positionB_) return;
        positionB_ = positionB;
        if (immediate) {
            fadeRemaining_ = 0;
            if (Effect* old = positionB ? a_.get() : b_.get()) old->reset();
        } else {
            fadeRemaining_ = fadeTotal_;
            // The one coming in starts clean (no stale echo tails).
            if (Effect* in = positionB ? b_.get() : a_.get()) in->reset();
        }
    }
    void process(float* left, float* right, int frames) override {
        Effect* in = positionB_ ? b_.get() : a_.get();
        Effect* out = positionB_ ? a_.get() : b_.get();
        if (fadeRemaining_ == 0) {
            if (in) in->process(left, right, frames);
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
            if (in) in->process(inL_.data(), inR_.data(), n);
            if (out) out->process(outL_.data(), outR_.data(), n);
            const float step = 1.0f / static_cast<float>(fadeTotal_);
            float g = 1.0f - static_cast<float>(fadeRemaining_) * step;  // weight of the new one
            for (int i = 0; i < n; ++i) {
                g = std::min(1.0f, g + step);
                l[i] = g * inL_[i] + (1.0f - g) * outL_[i];
                r[i] = g * inR_[i] + (1.0f - g) * outR_[i];
            }
            fadeRemaining_ = std::max(0, fadeRemaining_ - n);
            done += n;
            if (fadeRemaining_ == 0) {
                if (out) out->reset();
                if (done < frames && in) in->process(left + done, right + done, frames - done);
                break;
            }
        }
    }

private:
    std::unique_ptr<Effect> a_, b_;
    bool positionB_ = false;
    int fadeRemaining_ = 0;
    int fadeTotal_ = 1;
    std::array<float, kMaxBlockFrames> inL_{}, inR_{}, outL_{}, outR_{};
};

std::string text(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

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

        if (def.type == "motor") {  // drives the voices: one per program, switched by itself
            if (chain->motor_) {
                warnings.push_back(fxWhere + " (motor): only one motor per program - ignored");
                continue;
            }
            auto effect = createEffect(def.type, params, fxWhere, warnings);
            if (!effect) continue;
            if (!def.switchParam.empty()) {
                if (effect->configureSwitch(def.switchParam, json::parse(def.switchA), json::parse(def.switchB)))
                    chain->hasSwitch_ = true;
                else
                    warnings.push_back(fxWhere + " (motor) switch: must be [\"state\", \"on\", \"off\"] (or off, on) - ignored");
            }
            chain->motor_ = static_cast<Motor*>(effect.get());
            chain->effects_.push_back(std::move(effect));
            continue;
        }

        if (def.switchParam.empty()) {
            if (auto effect = createEffect(def.type, params, fxWhere, warnings))
                chain->effects_.push_back(std::move(effect));
            continue;
        }

        const json a = json::parse(def.switchA), b = json::parse(def.switchB);
        const std::string& param = def.switchParam;
        const std::string swWhere = fxWhere + " (" + def.type + ") switch";

        if (param == "bypass") {
            if (!a.is_boolean() || !b.is_boolean() || a == b) {
                warnings.push_back(swWhere + ": [\"bypass\", A, B] needs true and false - switch ignored");
                if (auto effect = createEffect(def.type, params, fxWhere, warnings))
                    chain->effects_.push_back(std::move(effect));
                continue;
            }
            auto effect = createEffect(def.type, params, fxWhere, warnings);
            if (!effect) continue;
            const bool bypassAtA = a.get<bool>();
            chain->effects_.push_back(bypassAtA ? std::make_unique<SwitchedEffect>(nullptr, std::move(effect))
                                                : std::make_unique<SwitchedEffect>(std::move(effect), nullptr));
            chain->hasSwitch_ = true;
            continue;
        }

        if (a == b) warnings.push_back(swWhere + ": the two values are the same");
        if (params.contains(param) && params[param] != a)
            warnings.push_back(swWhere + ": \"" + param + "\" is " + text(params[param]) +
                               " in params but the switch starts from " + text(a) + " - " + text(a) + " used");
        json paramsA = params, paramsB = params;
        paramsA[param] = a;
        paramsB[param] = b;
        auto effectA = createEffect(def.type, paramsA, fxWhere, warnings);
        if (!effectA) continue;
        if (effectA->configureSwitch(param, a, b)) {  // the effect changes it by itself
            chain->effects_.push_back(std::move(effectA));
            chain->hasSwitch_ = true;
            continue;
        }
        // Second instance with the B value; report only what concerns that value.
        std::vector<std::string> bWarnings;
        auto effectB = createEffect(def.type, paramsB, fxWhere, bWarnings);
        for (const auto& w : bWarnings)
            if (w.find("\"" + param + "\"") != std::string::npos) warnings.push_back(w + " (switch value B)");
        chain->effects_.push_back(std::make_unique<SwitchedEffect>(std::move(effectA), std::move(effectB)));
        chain->hasSwitch_ = true;
    }
    return chain;
}

void FxChain::prepare(double sampleRate) {
    for (auto& e : effects_) e->prepare(sampleRate);
    prepared_ = true;
}

void FxChain::reset() {
    for (auto& e : effects_) e->reset();
}

void FxChain::setSwitch(bool positionB, bool immediate) {
    for (auto& e : effects_) e->setSwitch(positionB, immediate);
}

void FxChain::process(float* left, float* right, int frames) {
    for (auto& e : effects_) e->process(left, right, frames);
}

} // namespace mbx
