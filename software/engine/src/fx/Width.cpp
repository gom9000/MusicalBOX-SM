#include "fx/Effects.h"

namespace mbx {

using dsp::Biquad;

Width::Width(FxParams& p)
    : amount_(p.number("amount", 0.5f, 0.0f, 1.0f)),
      timeMs_(p.number("time_ms", 12.0f, 3.0f, 30.0f)),
      lowCutHz_(p.number("low_cut_hz", 250.0f, 50.0f, 2000.0f)) {}

void Width::prepare(double sr) {
    delaySamples_ = static_cast<float>(timeMs_ * 0.001 * sr);
    line_.resize(static_cast<std::size_t>(delaySamples_) + 4);
    lowCut_.set(Biquad::Type::HighPass, sr, lowCutHz_, 0.707);  // the bass stays in the centre
    reset();
}

void Width::reset() {
    line_.reset();
    lowCut_.reset();
}

void Width::process(float* left, float* right, int frames) {
    const float a = 0.7f * amount_;
    for (int i = 0; i < frames; ++i) {
        const float mid = 0.5f * (left[i] + right[i]);
        line_.push(lowCut_.process(mid));
        const float side = a * line_.read(delaySamples_);
        left[i] += side;
        right[i] -= side;
    }
}

} // namespace mbx
