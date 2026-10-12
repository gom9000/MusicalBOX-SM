#include "fx/Effects.h"

#include <cmath>

namespace mbx {

using dsp::Biquad;

namespace {
// Q of the four sections of an 8th-order Butterworth low-pass.
constexpr double kButterworth8Q[4] = {0.5098, 0.6013, 0.9000, 2.5629};
} // namespace

void Fuzz::allocate(double sr) {
    const double osr = sr * kOversample;
    for (int k = 0; k < kFilterStages; ++k) {
        up_[k].set(Biquad::Type::LowPass, osr, 0.38 * sr, kButterworth8Q[k]);
        down_[k].set(Biquad::Type::LowPass, osr, 0.38 * sr, kButterworth8Q[k]);
    }
    octaveDc_.set(Biquad::Type::HighPass, osr, 30.0, 0.707);  // centres the rectified wave
    dcBlock_.set(Biquad::Type::HighPass, sr, 25.0, 0.707);
}

void Fuzz::update(bool) {
    const double sr = sampleRate_;
    // drive 0..1 -> 2x .. about 400x (the pedal is all gain).
    preGain_ = 2.0f * std::pow(200.0f, param(kDrive));
    // An asymmetric transistor: the curve is shifted, adding even harmonics.
    biasOffset_ = 0.4f * param(kBias);
    lowCut_.set(Biquad::Type::HighPass, sr, param(kLowCut), 0.707);
    tone_.set(Biquad::Type::LowPass, sr, param(kTone), 0.707);
    level_ = dsp::dbToGain(param(kLevel));
}

void Fuzz::reset() {
    lowCut_.reset(); octaveDc_.reset(); dcBlock_.reset(); tone_.reset();
    for (auto& f : up_) f.reset();
    for (auto& f : down_) f.reset();
}

void Fuzz::process(float* left, float* right, int frames) {
    // The clipper's output is about +-1 whatever the input: bring it to the
    // level of a typical sample, so blend and level work around 0 dB.
    constexpr float kFuzzLevel = 0.3f;
    const float clipShift = dsp::fastTanh(biasOffset_);
    const float octave = param(kOctave), blend = param(kBlend);
    for (int i = 0; i < frames; ++i) {
        const float dry = 0.5f * (left[i] + right[i]);
        const float in = lowCut_.process(dry);

        float y = 0.0f;
        for (int k = 0; k < kOversample; ++k) {
            // Zero-stuffing upsampler (gain kOversample on the first sub-sample).
            float s = k == 0 ? in * kOversample : 0.0f;
            for (auto& f : up_) s = f.process(s);

            // Fuzz: hard, slightly asymmetric clipping.
            const float fuzz = dsp::fastTanh(s * preGain_ + biasOffset_) - clipShift;
            // Octave: the rectified, gently driven wave (twice the frequency),
            // centred and squared up a little, as an octave-fuzz pedal's octave circuit.
            const float rect = octaveDc_.process(std::fabs(dsp::fastTanh(4.0f * s)));
            const float oct = dsp::fastTanh(3.0f * rect);
            float mixed = (1.0f - octave) * fuzz + octave * 1.4f * oct;

            for (auto& f : down_) mixed = f.process(mixed);
            y = mixed;  // the last sub-sample: decimation
        }
        const float fuzz = tone_.process(dcBlock_.process(y)) * kFuzzLevel;
        const float wet = blend * fuzz * level_;
        left[i] = (1.0f - blend) * left[i] + wet;
        right[i] = (1.0f - blend) * right[i] + wet;
    }
}

} // namespace mbx
