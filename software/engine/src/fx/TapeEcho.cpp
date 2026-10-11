#include "fx/Effects.h"

namespace mbx {

using dsp::Biquad;

TapeEcho::TapeEcho(FxParams& p)
    : timeMs_(p.number("time_ms", 350.0f, 20.0f, 2000.0f)),
      feedback_(p.number("feedback", 0.35f, 0.0f, 0.95f)),
      mix_(p.number("mix", 0.3f, 0.0f, 1.0f)),
      toneHz_(p.number("tone_hz", 2800.0f, 500.0f, 12000.0f)),
      lowCutHz_(p.number("low_cut_hz", 120.0f, 20.0f, 1000.0f)),
      wow_(p.number("wow", 0.3f, 0.0f, 1.0f)),
      drive_(p.number("drive", 0.3f, 0.0f, 1.0f)),
      pingPong_(p.flag("ping_pong", false)) {}

void TapeEcho::prepare(double sr) {
    delaySamples_ = static_cast<float>(timeMs_ * 0.001 * sr);
    wowDepth_ = static_cast<float>(wow_ * 0.001 * sr);        // up to 1 ms at 0.5 Hz (about 0.3% pitch)
    flutterDepth_ = static_cast<float>(wow_ * 0.00002 * sr);  // about 1 sample at 6 Hz
    for (int c = 0; c < 2; ++c) {
        line_[c].resize(static_cast<std::size_t>(delaySamples_ + wowDepth_ + flutterDepth_) + 8);
        dark_[c].set(Biquad::Type::LowPass, sr, toneHz_, 0.707);
        thin_[c].set(Biquad::Type::HighPass, sr, lowCutHz_, 0.707);
    }
    wowLfo_.set(sr, 0.5);
    flutterLfo_.set(sr, 6.1, 0.3);
    reset();
}

void TapeEcho::reset() {
    for (int c = 0; c < 2; ++c) {
        line_[c].reset();
        dark_[c].reset();
        thin_[c].reset();
    }
    wowLfo_.reset();
    flutterLfo_.reset();
}

void TapeEcho::process(float* left, float* right, int frames) {
    const float k = 1.0f + 3.0f * drive_;
    auto saturate = [k](float x) { return dsp::softClip(x * k) / k; };  // unity for small signals

    for (int i = 0; i < frames; ++i) {
        const float in = 0.5f * (left[i] + right[i]);
        const float d = delaySamples_ + wowDepth_ * wowLfo_.sine() + flutterDepth_ * flutterLfo_.sine();
        wowLfo_.advance();
        flutterLfo_.advance();

        if (!pingPong_) {
            const float e = thin_[0].process(dark_[0].process(line_[0].read(d)));
            line_[0].push(saturate(in + feedback_ * e));
            left[i] += mix_ * e;
            right[i] += mix_ * e;
        } else {
            const float eL = thin_[0].process(dark_[0].process(line_[0].read(d)));
            const float eR = thin_[1].process(dark_[1].process(line_[1].read(d)));
            line_[0].push(saturate(in + feedback_ * eR));
            line_[1].push(saturate(feedback_ * eL));
            left[i] += mix_ * eL;
            right[i] += mix_ * eR;
        }
    }
}

} // namespace mbx
