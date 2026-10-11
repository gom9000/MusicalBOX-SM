#pragma once

#include <array>

namespace mbx {

// Stereo-linked look-ahead peak limiter for the master output.
//
// When many voices add up above the threshold, the gain goes down smoothly
// just before the peak (look-ahead) and comes back with a slow release,
// instead of clipping the waveform. Below the threshold it does nothing.
//
// How it works, per sample:
//   1. required gain r = threshold / peak (1 when below the threshold);
//   2. minimum of r over the last kLookahead samples (sliding window);
//   3. release: the gain may rise only slowly (exponential);
//   4. box average over kLookahead samples, so the gain never jumps;
//   5. the audio is delayed by kLookahead - 1 samples, so the gain is
//      already low when the peak comes out.
// Steps 2 and 4 together guarantee |output| <= threshold.
//
// Real-time safe: fixed buffers, no allocation.
class Limiter {
public:
    static constexpr int kLookahead = 64;   // about 1.3 ms at 48 kHz

    void prepare(double sampleRate, float thresholdDb = -1.0f, float releaseMs = 120.0f);
    void reset();

    // In place, interleaved stereo. Returns the lowest gain applied in the block (1 = untouched).
    float process(float* interleavedStereo, int frames);

private:
    float threshold_ = 0.89f;
    float releaseCoef_ = 0.0f;

    // Delay line for the audio.
    std::array<float, 2 * kLookahead> delay_{};
    int delayPos_ = 0;

    // Sliding minimum (monotonic queue over a ring of kLookahead entries).
    std::array<float, kLookahead> minValue_{};
    std::array<long long, kLookahead> minIndex_{};
    int minHead_ = 0;
    int minCount_ = 0;
    long long sampleIndex_ = 0;

    float released_ = 1.0f;

    // Box average.
    std::array<float, kLookahead> box_{};
    int boxPos_ = 0;
    float boxSum_ = static_cast<float>(kLookahead);
};

} // namespace mbx
