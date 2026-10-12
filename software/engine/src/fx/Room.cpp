#include "fx/Effects.h"

#include <cmath>

namespace mbx {

using dsp::Biquad;

namespace {
// Line lengths in ms: mutually prime in samples at any common rate, spread so
// that the echoes are dense from the start (a small room, not a hall).
constexpr float kLineMs[] = {17.3f, 21.1f, 25.7f, 29.3f, 33.1f, 37.9f, 41.3f, 47.1f};
// Output taps: left and right read the lines with orthogonal sign patterns,
// so the two channels are uncorrelated.
constexpr float kOutL[] = {1, 1, 1, 1, -1, -1, -1, -1};
constexpr float kOutR[] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr float kIn[] = {1, -1, -1, 1, 1, 1, -1, -1};

// In-place 8-point Hadamard transform, scaled to stay energy-preserving.
inline void hadamard(float* x) {
    for (int h = 1; h < 8; h <<= 1)
        for (int i = 0; i < 8; i += 2 * h)
            for (int j = i; j < i + h; ++j) {
                const float a = x[j], b = x[j + h];
                x[j] = a + b;
                x[j + h] = a - b;
            }
    const float k = 0.35355339f;  // 1 / sqrt(8)
    for (int i = 0; i < 8; ++i) x[i] *= k;
}
} // namespace

void Room::allocate(double sr) {
    pre_.resize(static_cast<std::size_t>(80.0 * 0.001 * sr) + 4);  // the longest predelay
    for (int i = 0; i < kLines; ++i) {
        length_[i] = std::round(kLineMs[i] * 0.001f * static_cast<float>(sr));
        lines_[i].resize(static_cast<std::size_t>(length_[i]) + 4);
    }
}

void Room::update(bool) {
    const double sr = sampleRate_;
    const float timeS = param(kTime);
    predelay_ = static_cast<float>(param(kPredelay) * 0.001 * sr);
    lowCut_.set(Biquad::Type::HighPass, sr, param(kLowCut), 0.7071);
    for (int i = 0; i < kLines; ++i) {
        // 60 dB down after time_s: each pass through a line loses its share.
        gain_[i] = std::pow(10.0f, -3.0f * length_[i] / (timeS * static_cast<float>(sr)));
    }
    dampCoef_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979323846 * param(kDamp) / sr));
    // The tail of a longer room carries more energy: keep "level" about the
    // ratio between the room and the dry sound.
    outGain_ = 0.5f / std::sqrt(timeS / 0.7f);
}

void Room::reset() {
    pre_.reset();
    lowCut_.reset();
    for (auto& l : lines_) l.reset();
    damp_.fill(0.0f);
}

void Room::process(float* left, float* right, int frames) {
    float x[kLines];
    const float level = param(kLevel);
    for (int i = 0; i < frames; ++i) {
        pre_.push(lowCut_.process(0.5f * (left[i] + right[i])));
        const float in = pre_.read(predelay_);  // at least one sample

        float outL = 0.0f, outR = 0.0f;
        for (int k = 0; k < kLines; ++k) {
            float v = lines_[k].read(length_[k]);
            damp_[k] += dampCoef_ * (v - damp_[k]);  // the walls absorb the treble at every bounce
            v = damp_[k] * gain_[k];
            x[k] = v;
            outL += kOutL[k] * v;
            outR += kOutR[k] * v;
        }
        hadamard(x);
        for (int k = 0; k < kLines; ++k) lines_[k].push(x[k] + kIn[k] * in);

        left[i] += level * outGain_ * outL;
        right[i] += level * outGain_ * outR;
    }
}

} // namespace mbx
