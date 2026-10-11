#include "fx/Effects.h"

#include <cmath>

namespace mbx {

Chorus::Chorus(FxParams& p)
    : ensemble_(p.choice("mode", "ce2", {"ce2", "ensemble"}) == "ensemble"),
      rateHz_(p.number("rate_hz", ensemble_ ? 0.6f : 0.8f, 0.05f, 8.0f)),
      depth_(p.number("depth", 0.5f, 0.0f, 1.0f)),
      mix_(p.number("mix", ensemble_ ? 0.7f : 0.5f, 0.0f, 1.0f)) {}

void Chorus::prepare(double sr) {
    const double ms = 0.001 * sr;
    if (ensemble_) {
        baseDelay_ = static_cast<float>(12.0 * ms);
        slowDepth_ = static_cast<float>(depth_ * 4.0 * ms);   // slow, wide pitch drift
        fastDepth_ = static_cast<float>(depth_ * 0.35 * ms);  // fast shimmer
        fast_.set(sr, 6.3);
    } else {
        baseDelay_ = static_cast<float>(7.0 * ms);
        slowDepth_ = static_cast<float>(depth_ * 3.0 * ms);
        fastDepth_ = 0.0f;
    }
    slow_.set(sr, rateHz_);
    makeup_ = 1.0f / std::sqrt((1.0f - mix_) * (1.0f - mix_) + mix_ * mix_);
    for (int c = 0; c < 2; ++c) {
        line_[c].resize(static_cast<std::size_t>(baseDelay_ + slowDepth_ + fastDepth_) + 8);
        dark_[c].set(sr, 7000.0);  // bucket-brigade delays are dark
    }
    reset();
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
    const float dryGain = (1.0f - mix_) * makeup_, wetGain = mix_ * makeup_;
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
