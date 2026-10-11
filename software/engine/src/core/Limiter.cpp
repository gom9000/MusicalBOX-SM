#include "core/Limiter.h"

#include <algorithm>
#include <cmath>

namespace mbx {

void Limiter::prepare(double sampleRate, float thresholdDb, float releaseMs) {
    threshold_ = std::pow(10.0f, thresholdDb / 20.0f);
    releaseCoef_ = 1.0f - static_cast<float>(std::exp(-1.0 / (releaseMs * 0.001 * sampleRate)));
    reset();
}

void Limiter::reset() {
    delay_.fill(0.0f);
    delayPos_ = 0;
    minHead_ = 0;
    minCount_ = 0;
    sampleIndex_ = 0;
    released_ = 1.0f;
    box_.fill(1.0f);
    boxPos_ = 0;
    boxSum_ = static_cast<float>(kLookahead);
}

float Limiter::process(float* io, int frames) {
    float lowest = 1.0f;
    for (int i = 0; i < frames; ++i) {
        const float l = io[2 * i];
        const float r = io[2 * i + 1];

        // 1. Gain needed by this sample.
        const float peak = std::max(std::fabs(l), std::fabs(r));
        const float need = peak > threshold_ ? threshold_ / peak : 1.0f;

        // 2. Sliding minimum over the last kLookahead samples.
        if (minCount_ > 0 && minIndex_[minHead_] <= sampleIndex_ - kLookahead) {
            minHead_ = (minHead_ + 1) % kLookahead;
            --minCount_;
        }
        while (minCount_ > 0) {
            const int back = (minHead_ + minCount_ - 1) % kLookahead;
            if (minValue_[back] < need) break;
            --minCount_;
        }
        const int slot = (minHead_ + minCount_) % kLookahead;
        minValue_[slot] = need;
        minIndex_[slot] = sampleIndex_;
        ++minCount_;
        ++sampleIndex_;
        const float windowMin = minValue_[minHead_];

        // 3. Instant down, slow release up.
        released_ = std::min(windowMin, released_ + (1.0f - released_) * releaseCoef_);

        // 4. Box average (recomputed on each wrap so float error cannot pile up).
        boxSum_ += released_ - box_[boxPos_];
        box_[boxPos_] = released_;
        if (++boxPos_ == kLookahead) {
            boxPos_ = 0;
            float sum = 0.0f;
            for (float v : box_) sum += v;
            boxSum_ = sum;
        }
        const float gain = std::min(1.0f, boxSum_ / kLookahead);
        lowest = std::min(lowest, gain);

        // 5. Delay the audio by kLookahead - 1 samples and apply the gain.
        const int readPos = (delayPos_ + 1) % kLookahead;
        delay_[2 * delayPos_] = l;
        delay_[2 * delayPos_ + 1] = r;
        io[2 * i] = delay_[2 * readPos] * gain;
        io[2 * i + 1] = delay_[2 * readPos + 1] * gain;
        delayPos_ = readPos;
    }
    return lowest;
}

} // namespace mbx
