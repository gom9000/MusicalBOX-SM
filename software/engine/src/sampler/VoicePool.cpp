#include "sampler/VoicePool.h"

#include <algorithm>

namespace mbx {

void VoicePool::setPolyphony(int voices) {
    polyphony_ = std::clamp(voices, kMinPolyphony, kMaxPolyphony);
    voices_.assign(static_cast<std::size_t>(polyphony_ + kStealReserve), Voice{});
}

Voice* VoicePool::allocate(double engineRate) {
    int sounding = 0;
    Voice* oldestReleased = nullptr;
    Voice* oldest = nullptr;
    for (auto& v : voices_) {
        if (!v.active() || v.killed()) continue;
        ++sounding;
        if (!oldest || v.order() < oldest->order()) oldest = &v;
        if (v.released() && (!oldestReleased || v.order() < oldestReleased->order())) oldestReleased = &v;
    }
    if (sounding >= polyphony_) {
        Voice* victim = oldestReleased ? oldestReleased : oldest;
        if (victim) victim->kill(engineRate);
    }

    for (auto& v : voices_)
        if (!v.active()) return &v;

    // Every slot busy, even the reserve: reuse the oldest voice that is
    // already fading out (a hard cut, only with very dense playing).
    Voice* fading = nullptr;
    for (auto& v : voices_)
        if (v.killed() && (!fading || v.order() < fading->order())) fading = &v;
    return fading;
}

void VoicePool::render(int bus, float* left, float* right, int frames, const VoiceMod& mod) {
    for (auto& voice : voices_)
        if (voice.active() && voice.bus == bus) voice.render(left, right, frames, mod);
}

bool VoicePool::busActive(int bus) const {
    for (const auto& voice : voices_)
        if (voice.active() && voice.bus == bus) return true;
    return false;
}

int VoicePool::activeCount() const {
    int n = 0;
    for (const auto& voice : voices_)
        if (voice.active()) ++n;
    return n;
}

} // namespace mbx
