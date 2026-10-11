#pragma once

// Small DSP building blocks shared by the effects. Header-only, no
// allocation outside prepare-time resize().

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mbx::dsp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// tanh by a rational approximation (Lambert continued fraction): error below 1e-4,
// several times faster than std::tanh. Used where it runs many times
// per sample (the oversampled fuzz).
inline float fastTanh(float x) {
    if (x > 4.97f) return 1.0f;
    if (x < -4.97f) return -1.0f;
    const float x2 = x * x;
    const float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    const float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2));
    return num / den;
}

// Soft saturation with unity slope at 0 (tape, preamp).
inline float softClip(float x) { return std::tanh(x); }

// Circular delay line with fractional (linear) read.
class DelayLine {
public:
    void resize(std::size_t maxSamples) {
        std::size_t size = 1;
        while (size < maxSamples + 4) size <<= 1;
        buffer_.assign(size, 0.0f);
        mask_ = size - 1;
        write_ = 0;
    }
    void reset() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); }

    void push(float x) {
        buffer_[write_] = x;
        write_ = (write_ + 1) & mask_;
    }
    // Delay in samples (>= 1) measured from the last pushed sample.
    float read(float delay) const {
        const float d = std::max(1.0f, delay);
        const std::size_t whole = static_cast<std::size_t>(d);
        const float frac = d - static_cast<float>(whole);
        const float a = buffer_[(write_ - whole) & mask_];
        const float b = buffer_[(write_ - whole - 1) & mask_];
        return a + (b - a) * frac;
    }

private:
    std::vector<float> buffer_;
    std::size_t mask_ = 0;
    std::size_t write_ = 0;
};

// RBJ biquad, transposed direct form II.
class Biquad {
public:
    enum class Type { LowPass, HighPass, LowShelf, HighShelf, Peak };

    void set(Type type, double sampleRate, double freq, double q, double gainDb = 0.0) {
        const double f = std::clamp(freq, 10.0, 0.45 * sampleRate);
        const double w = 2.0 * 3.14159265358979323846 * f / sampleRate;
        const double cw = std::cos(w), sw = std::sin(w);
        const double alpha = sw / (2.0 * q);
        const double A = std::pow(10.0, gainDb / 40.0);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (type) {
        case Type::LowPass:
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case Type::HighPass:
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case Type::Peak:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case Type::LowShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case Type::HighShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        }
        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
    }
    void reset() { z1_ = z2_ = 0.0f; }
    float process(float x) {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_ = 0, z2_ = 0;
};

// One-pole low-pass (gentle darkening, smoothing).
class OnePole {
public:
    void set(double sampleRate, double freq) {
        a_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979323846 * freq / sampleRate));
    }
    void reset() { y_ = 0.0f; }
    float process(float x) { return y_ += a_ * (x - y_); }

private:
    float a_ = 1.0f;
    float y_ = 0.0f;
};

// Phase accumulator in [0, 1).
class Lfo {
public:
    void set(double sampleRate, double hz, double phase01 = 0.0) {
        inc_ = static_cast<float>(hz / sampleRate);
        start_ = static_cast<float>(phase01 - std::floor(phase01));
        phase_ = start_;
    }
    void reset() { phase_ = start_; }
    void setHz(double sampleRate, double hz) { inc_ = static_cast<float>(hz / sampleRate); }  // keeps the phase
    float phase() const { return phase_; }
    void advance() {
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
    // Waveforms at an extra phase offset, range -1..1.
    float sine(float offset = 0.0f) const { return std::sin(kTwoPi * (phase_ + offset)); }
    float triangle(float offset = 0.0f) const {
        float p = phase_ + offset;
        p -= std::floor(p);
        return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
    }

private:
    float inc_ = 0.0f;
    float phase_ = 0.0f;
    float start_ = 0.0f;
};

} // namespace mbx::dsp
