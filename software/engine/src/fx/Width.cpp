#include "fx/Effects.h"

namespace mbx {

using dsp::Biquad;

void Width::allocate(double sr) {
    line_.resize(static_cast<std::size_t>(30.0 * 0.001 * sr) + 4);  // the longest time
}

void Width::update(bool) {
    delaySamples_ = static_cast<float>(param(kTime) * 0.001 * sampleRate_);
    lowCut_.set(Biquad::Type::HighPass, sampleRate_, param(kLowCut), 0.707);  // the bass stays in the centre
}

void Width::reset() {
    line_.reset();
    lowCut_.reset();
}

void Width::process(float* left, float* right, int frames) {
    const float a = 0.7f * param(kAmount);
    for (int i = 0; i < frames; ++i) {
        const float mid = 0.5f * (left[i] + right[i]);
        line_.push(lowCut_.process(mid));
        const float side = a * line_.read(delaySamples_);
        left[i] += side;
        right[i] -= side;
    }
}

} // namespace mbx
