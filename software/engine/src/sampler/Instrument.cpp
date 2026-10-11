#include "sampler/Instrument.h"

#include <cstdlib>
#include <limits>

namespace mbx {

Instrument::Instrument(std::string id, std::string name)
    : id_(std::move(id)), name_(std::move(name)), map_(128 * 128, nullptr) {}

void Instrument::addZone(Zone zone) {
    zones_.push_back(std::make_unique<Zone>(std::move(zone)));
}

void Instrument::finalize(FillMode fill) {
    for (int note = 0; note < 128; ++note) {
        for (int vel = 0; vel < 128; ++vel) {
            const int v = vel == 0 ? 1 : vel;  // velocity 0 never plays; mirror 1
            const Zone* best = nullptr;
            int bestDistance = std::numeric_limits<int>::max();
            for (const auto& zone : zones_) {
                if (v < zone->loVel || v > zone->hiVel) continue;
                const int distance = std::abs(zone->rootNote - note);
                if (fill == FillMode::None && distance != 0) continue;
                if (distance < bestDistance ||
                    (distance == bestDistance && best && zone->rootNote < best->rootNote)) {
                    best = zone.get();
                    bestDistance = distance;
                }
            }
            if (!best) {
                // No zone covers this velocity (a layer without files): take the
                // nearest velocity layer, then the nearest note, instead of silence.
                int bestVel = std::numeric_limits<int>::max();
                for (const auto& zone : zones_) {
                    const int velDistance = v < zone->loVel ? zone->loVel - v : (v > zone->hiVel ? v - zone->hiVel : 0);
                    const int distance = std::abs(zone->rootNote - note);
                    if (fill == FillMode::None && distance != 0) continue;
                    if (velDistance < bestVel || (velDistance == bestVel && distance < bestDistance)) {
                        best = zone.get();
                        bestVel = velDistance;
                        bestDistance = distance;
                    }
                }
            }
            map_[note * 128 + vel] = best;
        }
    }
}

const Zone* Instrument::findZone(int note, int velocity) const {
    if (note < 0 || note > 127 || velocity < 0 || velocity > 127) return nullptr;
    return map_[note * 128 + velocity];
}

std::size_t Instrument::memoryBytes() const {
    std::size_t total = 0;
    for (const auto& zone : zones_) total += zone->memoryBytes();
    return total;
}

int Instrument::dualMonoCount() const {
    int n = 0;
    for (const auto& zone : zones_)
        if (zone->dualMono) ++n;
    return n;
}

bool Instrument::hasLoops() const {
    for (const auto& zone : zones_)
        if (zone->hasLoop) return true;
    return false;
}

} // namespace mbx
