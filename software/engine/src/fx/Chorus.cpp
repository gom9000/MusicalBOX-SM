#include "fx/Effects.h"

#include <cmath>

namespace mbx {

void Chorus::allocate(double sr) {
    const double ms = 0.001 * sr;
    // Room for the deepest setting of either mode.
    const double most = std::max(12.0 + 4.0 + 0.35, 7.0 + 3.0) * ms;
    for (int c = 0; c < 2; ++c) {
        line_[c].resize(static_cast<std::size_t>(most) + 8);
        dark_[c].set(sr, 7000.0);  // bucket-brigade delays are dark
    }
    ensemble_ = choice(kMode) == 1;
    if (ensemble_) fast_.set(sr, 6.3);
    slow_.set(sr, param(kRate));
}

void Chorus::update(bool) {
    const double ms = 0.001 * sampleRate_;
    const float depth = param(kDepth), mix = param(kMix);
    if (ensemble_) {
        baseDelay_ = static_cast<float>(12.0 * ms);
        slowDepth_ = static_cast<float>(depth * 4.0 * ms);   // slow, wide pitch drift
        fastDepth_ = static_cast<float>(depth * 0.35 * ms);  // fast shimmer
    } else {
        baseDelay_ = static_cast<float>(7.0 * ms);
        slowDepth_ = static_cast<float>(depth * 3.0 * ms);
        fastDepth_ = 0.0f;
    }
    slow_.setHz(sampleRate_, param(kRate));
    makeup_ = 1.0f / std::sqrt((1.0f - mix) * (1.0f - mix) + mix * mix);
}

void Chorus::reset() {
    for (int c = 0; c < 2; ++c) {
        line_[c].reset();
        dark_[c].reset();
    }
    slow_.reset();
    fast_.reset();
}

void Chorus::process(float* left, float* right, int frames) {
    float* io[2] = {left, right};
    // Dry and modulated sound are loosely correlated: keep the loudness of the dry sound.
    const float mix = param(kMix);
    const float dryGain = (1.0f - mix) * makeup_, wetGain = mix * makeup_;
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            const float dry = io[c][i];
            line_[c].push(dry);
            float wet;
            if (ensemble_) {
                // Three taps 120 degrees apart; the right channel is shifted by 60 degrees.
                const float shift = c == 0 ? 0.0f : 1.0f / 6.0f;
                wet = 0.0f;
                for (int t = 0; t < 3; ++t) {
                    const float off = shift + t / 3.0f;
                    const float d = baseDelay_ + slowDepth_ * slow_.sine(off) + fastDepth_ * fast_.sine(off);
                    wet += line_[c].read(d);
                }
                wet *= 0.57735f;  // 1/sqrt(3): three loosely correlated taps
            } else {
                // One tap, triangle LFO, opposite phase on the right for width.
                const float d = baseDelay_ + slowDepth_ * slow_.triangle(c == 0 ? 0.0f : 0.5f);
                wet = line_[c].read(d);
            }
            wet = dark_[c].process(wet);
            io[c][i] = dryGain * dry + wetGain * wet;
        }
        slow_.advance();
        fast_.advance();
    }
}

} // namespace mbx
