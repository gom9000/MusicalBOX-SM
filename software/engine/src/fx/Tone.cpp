#include "fx/Effects.h"

namespace mbx {

using dsp::Biquad;

void Tone::allocate(double) {}

void Tone::update(bool) {
    const double sr = sampleRate_;
    for (auto& c : ch_) {
        c.lowCut.set(Biquad::Type::HighPass, sr, param(kLowCut) > 0 ? param(kLowCut) : 10.0, 0.707);
        c.bass.set(Biquad::Type::LowShelf, sr, param(kBassHz), 0.707, param(kBassDb));
        c.mid.set(Biquad::Type::Peak, sr, param(kMidHz), 0.8, param(kMidDb));
        c.treble.set(Biquad::Type::HighShelf, sr, param(kTrebleHz), 0.707, param(kTrebleDb));
        c.highCut.set(Biquad::Type::LowPass, sr, param(kHighCut) > 0 ? param(kHighCut) : 0.45 * sr, 0.707);
    }
    gain_ = dsp::dbToGain(param(kGain));
}

void Tone::reset() {
    for (auto& c : ch_) {
        c.lowCut.reset(); c.bass.reset(); c.mid.reset(); c.treble.reset(); c.highCut.reset();
    }
}

void Tone::process(float* left, float* right, int frames) {
    float* io[2] = {left, right};
    const bool lowCut = param(kLowCut) > 0, bass = param(kBassDb) != 0, mid = param(kMidDb) != 0,
               treble = param(kTrebleDb) != 0, highCut = param(kHighCut) > 0;
    for (int c = 0; c < 2; ++c) {
        Channel& ch = ch_[c];
        float* x = io[c];
        for (int i = 0; i < frames; ++i) {
            float s = x[i];
            if (lowCut) s = ch.lowCut.process(s);
            if (bass) s = ch.bass.process(s);
            if (mid) s = ch.mid.process(s);
            if (treble) s = ch.treble.process(s);
            if (highCut) s = ch.highCut.process(s);
            x[i] = s * gain_;
        }
    }
}

} // namespace mbx
