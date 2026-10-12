#include "fx/Effects.h"

#include <cmath>

namespace mbx {

void Tremolo::allocate(double sr) { lfo_.set(sr, param(kRate)); }

void Tremolo::update(bool) {
    lfo_.setHz(sampleRate_, param(kRate));
    // Same average loudness as without the effect (RMS of the gain curve).
    const float depth = param(kDepth);
    const float mean = 1.0f - 0.5f * depth;
    makeup_ = 1.0f / std::sqrt(mean * mean + depth * depth / 8.0f);
}

void Tremolo::reset() { lfo_.reset(); }

void Tremolo::process(float* left, float* right, int frames) {
    static const float kSquareNorm = 1.0f / std::tanh(4.0f);
    const int shape = choice(kShape);
    const bool stereo = flag(kStereo);
    const float depth = param(kDepth);
    auto wave = [&](float offset) {
        switch (shape) {
        case 1: return lfo_.triangle(offset);
        case 2: return std::tanh(4.0f * lfo_.sine(offset)) * kSquareNorm;  // rounded square, no clicks
        default: return lfo_.sine(offset);
        }
    };
    for (int i = 0; i < frames; ++i) {
        const float a = wave(0.0f);
        const float b = stereo ? wave(0.5f) : a;  // pan tremolo: the right side is in opposite phase
        left[i] *= makeup_ * (1.0f - depth * (0.5f - 0.5f * a));
        right[i] *= makeup_ * (1.0f - depth * (0.5f - 0.5f * b));
        lfo_.advance();
    }
}

} // namespace mbx
