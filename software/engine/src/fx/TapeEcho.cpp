#include "fx/Effects.h"

namespace mbx {

using dsp::Biquad;

void TapeEcho::allocate(double sr) {
    // Room for the longest time and the deepest wow, so time and wow can move while it plays.
    const float most = static_cast<float>(2000.0 * 0.001 * sr + 1.0 * 0.001 * sr + 1.0 * 0.00002 * sr);
    for (int c = 0; c < 2; ++c) line_[c].resize(static_cast<std::size_t>(most) + 8);
    wowLfo_.set(sr, 0.5);
    flutterLfo_.set(sr, 6.1, 0.3);
}

void TapeEcho::update(bool) {
    const double sr = sampleRate_;
    delaySamples_ = static_cast<float>(param(kTime) * 0.001 * sr);
    wowDepth_ = static_cast<float>(param(kWow) * 0.001 * sr);        // up to 1 ms at 0.5 Hz (about 0.3% pitch)
    flutterDepth_ = static_cast<float>(param(kWow) * 0.00002 * sr);  // about 1 sample at 6 Hz
    for (int c = 0; c < 2; ++c) {
        dark_[c].set(Biquad::Type::LowPass, sr, param(kTone), 0.707);
        thin_[c].set(Biquad::Type::HighPass, sr, param(kLowCut), 0.707);
    }
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
    const float feedback = param(kFeedback), mix = param(kMix);
    const float k = 1.0f + 3.0f * param(kDrive);
    auto saturate = [k](float x) { return dsp::softClip(x * k) / k; };  // unity for small signals

    for (int i = 0; i < frames; ++i) {
        const float in = 0.5f * (left[i] + right[i]);
        const float d = delaySamples_ + wowDepth_ * wowLfo_.sine() + flutterDepth_ * flutterLfo_.sine();
        wowLfo_.advance();
        flutterLfo_.advance();

        if (!flag(kPingPong)) {
            const float e = thin_[0].process(dark_[0].process(line_[0].read(d)));
            line_[0].push(saturate(in + feedback * e));
            left[i] += mix * e;
            right[i] += mix * e;
        } else {
            const float eL = thin_[0].process(dark_[0].process(line_[0].read(d)));
            const float eR = thin_[1].process(dark_[1].process(line_[1].read(d)));
            line_[0].push(saturate(in + feedback * eR));
            line_[1].push(saturate(feedback * eL));
            left[i] += mix * eL;
            right[i] += mix * eR;
        }
    }
}

} // namespace mbx
