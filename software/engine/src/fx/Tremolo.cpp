#include "fx/Effects.h"

#include <cmath>

namespace mbx {

Tremolo::Tremolo(FxParams& p)
    : rateHz_(p.number("rate_hz", 5.0f, 0.1f, 20.0f)),
      depth_(p.number("depth", 0.5f, 0.0f, 1.0f)) {
    const std::string shape = p.choice("shape", "sine", {"sine", "triangle", "square"});
    shape_ = shape == "triangle" ? 1 : shape == "square" ? 2 : 0;
    stereo_ = p.flag("stereo", false);
}

void Tremolo::prepare(double sr) {
    lfo_.set(sr, rateHz_);
    // Same average loudness as without the effect (RMS of the gain curve).
    const float mean = 1.0f - 0.5f * depth_;
    makeup_ = 1.0f / std::sqrt(mean * mean + depth_ * depth_ / 8.0f);
    reset();
}

void Tremolo::reset() { lfo_.reset(); }

void Tremolo::process(float* left, float* right, int frames) {
    static const float kSquareNorm = 1.0f / std::tanh(4.0f);
    auto wave = [&](float offset) {
        switch (shape_) {
        case 1: return lfo_.triangle(offset);
        case 2: return std::tanh(4.0f * lfo_.sine(offset)) * kSquareNorm;  // rounded square, no clicks
        default: return lfo_.sine(offset);
        }
    };
    for (int i = 0; i < frames; ++i) {
        const float a = wave(0.0f);
        const float b = stereo_ ? wave(0.5f) : a;  // pan tremolo: the right side is in opposite phase
        left[i] *= makeup_ * (1.0f - depth_ * (0.5f - 0.5f * a));
        right[i] *= makeup_ * (1.0f - depth_ * (0.5f - 0.5f * b));
        lfo_.advance();
    }
}

} // namespace mbx
