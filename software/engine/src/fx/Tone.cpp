#include "fx/Effects.h"

namespace mbx {

using dsp::Biquad;

Tone::Tone(FxParams& p)
    : lowCutHz_(p.number("low_cut_hz", 0.0f, 0.0f, 1000.0f)),
      bassDb_(p.number("bass_db", 0.0f, -18.0f, 18.0f)),
      bassHz_(p.number("bass_hz", 120.0f, 30.0f, 500.0f)),
      midDb_(p.number("mid_db", 0.0f, -18.0f, 18.0f)),
      midHz_(p.number("mid_hz", 1000.0f, 200.0f, 5000.0f)),
      trebleDb_(p.number("treble_db", 0.0f, -18.0f, 18.0f)),
      trebleHz_(p.number("treble_hz", 3500.0f, 1000.0f, 12000.0f)),
      highCutHz_(p.number("high_cut_hz", 0.0f, 0.0f, 20000.0f)),
      gain_(dsp::dbToGain(p.number("gain_db", 0.0f, -24.0f, 12.0f))) {}

void Tone::prepare(double sr) {
    for (auto& c : ch_) {
        c.lowCut.set(Biquad::Type::HighPass, sr, lowCutHz_ > 0 ? lowCutHz_ : 10.0, 0.707);
        c.bass.set(Biquad::Type::LowShelf, sr, bassHz_, 0.707, bassDb_);
        c.mid.set(Biquad::Type::Peak, sr, midHz_, 0.8, midDb_);
        c.treble.set(Biquad::Type::HighShelf, sr, trebleHz_, 0.707, trebleDb_);
        c.highCut.set(Biquad::Type::LowPass, sr, highCutHz_ > 0 ? highCutHz_ : 0.45 * sr, 0.707);
    }
    reset();
}

void Tone::reset() {
    for (auto& c : ch_) {
        c.lowCut.reset(); c.bass.reset(); c.mid.reset(); c.treble.reset(); c.highCut.reset();
    }
}

void Tone::process(float* left, float* right, int frames) {
    float* io[2] = {left, right};
    const bool lowCut = lowCutHz_ > 0, bass = bassDb_ != 0, mid = midDb_ != 0, treble = trebleDb_ != 0,
               highCut = highCutHz_ > 0;
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
