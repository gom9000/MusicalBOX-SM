#include "fx/Effects.h"

#include <cmath>

namespace mbx {

Phaser::Phaser(FxParams& p)
    : rateHz_(p.number("rate_hz", 0.5f, 0.05f, 5.0f)),
      depth_(p.number("depth", 1.0f, 0.0f, 1.0f)),
      feedback_(p.number("feedback", 0.2f, 0.0f, 0.8f)),
      mix_(p.number("mix", 0.5f, 0.0f, 1.0f)),
      stages_(2 * static_cast<int>(std::lround(p.number("stages", 4.0f, 2.0f, 12.0f) / 2.0f))),
      stereo_(p.flag("stereo", false)) {}

void Phaser::prepare(double sr) {
    sampleRate_ = sr;
    lfo_.set(sr, rateHz_);
    // Dry + phase-shifted sum loses about 3 dB on average at mix 0.5: compensate.
    makeup_ = 1.0f / std::sqrt((1.0f - mix_) * (1.0f - mix_) + mix_ * mix_);
    reset();
}

void Phaser::reset() {
    for (auto& c : ch_) {
        c.x1.fill(0.0f);
        c.y1.fill(0.0f);
        c.last = 0.0f;
    }
    lfo_.reset();
    controlCounter_ = 0;
}

void Phaser::process(float* left, float* right, int frames) {
    constexpr int kControlEvery = 16;  // the sweep is slow: update the all-pass every 16 samples
    const float lo = 200.0f;
    const float range = std::pow(8.0f, depth_);  // depth 1: 200 Hz .. 1.6 kHz
    float* io[2] = {left, right};

    for (int i = 0; i < frames; ++i) {
        if (controlCounter_ == 0) {
            for (int c = 0; c < 2; ++c) {
                const float offset = (stereo_ && c == 1) ? 0.25f : 0.0f;
                const float sweep = 0.5f + 0.5f * lfo_.sine(offset);
                const float f = lo * std::pow(range, sweep);
                const float t = std::tan(dsp::kPi * f / static_cast<float>(sampleRate_));
                ch_[c].coef = (t - 1.0f) / (t + 1.0f);
            }
        }
        controlCounter_ = (controlCounter_ + 1) % kControlEvery;
        lfo_.advance();

        for (int c = 0; c < 2; ++c) {
            Channel& ch = ch_[c];
            const float dry = io[c][i];
            float s = dry + feedback_ * ch.last;
            for (int k = 0; k < stages_; ++k) {
                const float y = ch.coef * s + ch.x1[k] - ch.coef * ch.y1[k];
                ch.x1[k] = s;
                ch.y1[k] = y;
                s = y;
            }
            ch.last = s;
            io[c][i] = makeup_ * ((1.0f - mix_) * dry + mix_ * s);
        }
    }
}

} // namespace mbx
