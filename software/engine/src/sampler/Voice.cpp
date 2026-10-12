#include "sampler/Voice.h"

#include "core/Constants.h"
#include "sampler/Program.h"

#include <cmath>

namespace mbx {

void Voice::start(const Zone& zone, const Layer& layer, int key, int playedNote, float gain,
                  double engineRate, std::uint64_t order) {
    zone_ = &zone;
    pos_ = 0.0;
    // Pitch shift from the zone's root note, plus the file/engine rate ratio:
    // this is how a 44.1 kHz sample plays correctly on a 48 kHz engine.
    step_ = std::pow(2.0, (playedNote - zone.rootNote) / 12.0) * (zone.sampleRate / engineRate);
    gain_ = gain;
    loop_ = layer.loop && zone.hasLoop;
    key_ = key;
    order_ = order;
    sustained = false;
    killed_ = false;
    active_ = true;
    env_.setup(layer.adsr, engineRate);
    env_.noteOn();
}

void Voice::release() {
    env_.noteOff();
}

void Voice::kill(double engineRate) {
    killed_ = true;
    env_.fastRelease(engineRate, kKillFadeMs);
}

void Voice::render(float* left, float* right, int frames, const VoiceMod& mod) {
    if (!active_) return;
    if (zone_->channels == 2)
        renderImpl<2>(left, right, frames, mod);
    else
        renderImpl<1>(left, right, frames, mod);
}

template <int Channels>
void Voice::renderImpl(float* left, float* right, int frames, const VoiceMod& mod) {
    const Zone& z = *zone_;
    const std::int16_t* d = z.data.data();
    const std::uint32_t last = z.frames - 1;
    const double loopLimit = static_cast<double>(z.loopEnd) + 1.0;
    const double loopLength = static_cast<double>(z.loopEnd) + 1.0 - static_cast<double>(z.loopStart);
    const float scale = gain_ / 32768.0f;
    const bool modulated = mod.stepFrom != 1.0f || mod.stepTo != 1.0f || mod.gainFrom != 1.0f || mod.gainTo != 1.0f;
    const double stepInc = (static_cast<double>(mod.stepTo) - mod.stepFrom) / frames;
    const float gainInc = (mod.gainTo - mod.gainFrom) / static_cast<float>(frames);
    double stepMul = mod.stepFrom;
    float gainMul = mod.gainFrom;

    for (int i = 0; i < frames; ++i) {
        if (env_.idle()) {
            active_ = false;
            return;
        }
        const auto k = static_cast<std::uint32_t>(pos_);
        if (!loop_ && k >= last) {  // end of a one-shot sample
            active_ = false;
            return;
        }
        std::uint32_t k1 = k + 1;
        if (loop_ && k1 > z.loopEnd) k1 = z.loopStart;

        const float frac = static_cast<float>(pos_ - k);
        const float amp = modulated ? env_.next() * scale * gainMul : env_.next() * scale;

        if constexpr (Channels == 1) {
            const float a = d[k];
            const float s = a + (static_cast<float>(d[k1]) - a) * frac;
            left[i] += s * amp;
            right[i] += s * amp;
        } else {
            const float al = d[2 * k];
            const float ar = d[2 * k + 1];
            left[i] += (al + (static_cast<float>(d[2 * k1]) - al) * frac) * amp;
            right[i] += (ar + (static_cast<float>(d[2 * k1 + 1]) - ar) * frac) * amp;
        }

        if (modulated) {
            pos_ += step_ * stepMul;
            stepMul += stepInc;
            gainMul += gainInc;
        } else {
            pos_ += step_;
        }
        if (loop_) {
            while (pos_ >= loopLimit) pos_ -= loopLength;
        }
    }
}

} // namespace mbx
