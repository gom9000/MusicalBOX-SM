#pragma once

#include "sampler/Envelope.h"
#include "sampler/Zone.h"

#include <cstdint>

namespace mbx {

struct Layer;

// One sounding note: a read position in a zone, a playback speed and an
// envelope. Renders by adding into the output buffers. No allocation.
class Voice {
public:
    // key:        the note received (used to match note-off);
    // playedNote: key + layer transpose (sets the pitch).
    void start(const Zone& zone, const Layer& layer, int key, int playedNote, float gain,
               double engineRate, std::uint64_t order);
    void release();
    void kill(double engineRate);  // short fade, then the slot is free

    void render(float* left, float* right, int frames);

    bool active() const { return active_; }
    bool released() const { return env_.stage() == Envelope::Stage::Release; }
    bool killed() const { return killed_; }
    int key() const { return key_; }
    std::uint64_t order() const { return order_; }

    bool sustained = false;  // note-off arrived while the pedal was down
    int bus = 0;             // effect chain the voice plays into (0 or 1, see Engine)

private:
    template <int Channels>
    void renderImpl(float* left, float* right, int frames);

    const Zone* zone_ = nullptr;
    double pos_ = 0.0;
    double step_ = 1.0;
    float gain_ = 1.0f;
    bool loop_ = false;
    bool active_ = false;
    bool killed_ = false;
    int key_ = -1;
    std::uint64_t order_ = 0;
    Envelope env_;
};

} // namespace mbx
