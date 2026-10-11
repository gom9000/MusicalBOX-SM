#include "sampler/Envelope.h"

#include <algorithm>
#include <cmath>

namespace mbx {
namespace {

double samplesFor(float ms, double sampleRate) {
    return std::max(1.0, static_cast<double>(ms) * sampleRate / 1000.0);
}

// Per-sample multiplier that brings a level from 1 down to `target` in `samples`.
float coefficient(double samples, double target) {
    return static_cast<float>(std::exp(std::log(target) / samples));
}

} // namespace

void Envelope::setup(const Adsr& adsr, double sampleRate) {
    attackStep_ = static_cast<float>(1.0 / samplesFor(adsr.attackMs, sampleRate));
    decayCoef_ = adsr.decayMs > 0.0f ? coefficient(samplesFor(adsr.decayMs, sampleRate), 1e-3) : 0.0f;
    sustain_ = std::clamp(adsr.sustain, 0.0f, 1.0f);
    releaseCoef_ = coefficient(samplesFor(adsr.releaseMs, sampleRate), kSilence);
    stage_ = Stage::Idle;
    level_ = 0.0f;
}

void Envelope::noteOn() {
    level_ = 0.0f;
    stage_ = Stage::Attack;
}

void Envelope::noteOff() {
    if (stage_ != Stage::Idle) stage_ = Stage::Release;
}

void Envelope::fastRelease(double sampleRate, float ms) {
    if (stage_ == Stage::Idle) return;
    releaseCoef_ = coefficient(samplesFor(ms, sampleRate), kSilence);
    stage_ = Stage::Release;
}

} // namespace mbx
