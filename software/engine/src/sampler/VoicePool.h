#pragma once

#include "core/Constants.h"
#include "sampler/Voice.h"

#include <cstdint>
#include <vector>

namespace mbx {

// Fixed set of voices, allocated once before the audio starts.
// When the polyphony is exceeded, the oldest voice (preferring one already
// released) is faded out in 5 ms instead of being cut, and the new note
// takes one of the kStealReserve spare slots.
class VoicePool {
public:
    VoicePool() { setPolyphony(kDefaultPolyphony); }

    // Allocates the voices. Not real-time safe: call before the audio starts.
    void setPolyphony(int voices);
    int polyphony() const { return polyphony_; }

    Voice* allocate(double engineRate);
    // Adds the voices playing into `bus` to the buffers.
    void render(int bus, float* left, float* right, int frames);
    bool busActive(int bus) const;
    int activeCount() const;

    template <typename F>
    void forEachActive(F&& f) {
        for (auto& voice : voices_)
            if (voice.active()) f(voice);
    }

    std::uint64_t nextOrder() { return ++counter_; }

private:
    std::vector<Voice> voices_;
    int polyphony_ = 0;
    std::uint64_t counter_ = 0;
};

} // namespace mbx
