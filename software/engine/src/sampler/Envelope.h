#pragma once

#include "sampler/Program.h"

namespace mbx {

// Per-voice ADSR. Linear attack; decay and release are exponential
// (they sound natural and never click). Advanced once per sample.
class Envelope {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setup(const Adsr& adsr, double sampleRate);
    void noteOn();
    void noteOff();
    // Short fade to silence, used when a voice must be cut.
    void fastRelease(double sampleRate, float ms);

    float next() {
        switch (stage_) {
        case Stage::Attack:
            level_ += attackStep_;
            if (level_ >= 1.0f) {
                level_ = 1.0f;
                stage_ = (decayCoef_ > 0.0f && sustain_ < 1.0f) ? Stage::Decay : Stage::Sustain;
                if (stage_ == Stage::Sustain) level_ = sustain_;
            }
            break;
        case Stage::Decay:
            level_ = sustain_ + (level_ - sustain_) * decayCoef_;
            if (level_ - sustain_ < 1e-4f) {
                level_ = sustain_;
                stage_ = Stage::Sustain;
            }
            break;
        case Stage::Sustain:
            break;
        case Stage::Release:
            level_ *= releaseCoef_;
            if (level_ < kSilence) {
                level_ = 0.0f;
                stage_ = Stage::Idle;
            }
            break;
        case Stage::Idle:
            level_ = 0.0f;
            break;
        }
        return level_;
    }

    bool idle() const { return stage_ == Stage::Idle; }
    Stage stage() const { return stage_; }
    float level() const { return level_; }

private:
    static constexpr float kSilence = 1e-4f;  // -80 dB

    Stage stage_ = Stage::Idle;
    float level_ = 0.0f;
    float attackStep_ = 1.0f;
    float decayCoef_ = 0.0f;
    float sustain_ = 1.0f;
    float releaseCoef_ = 0.0f;
};

} // namespace mbx
