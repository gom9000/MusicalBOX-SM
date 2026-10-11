#pragma once

#include "sampler/Zone.h"

#include <memory>
#include <string>
#include <vector>

namespace mbx {

// How notes without their own file are served.
enum class FillMode {
    Nearest,  // use the closest zone, transposed (ties: the lower one)
    None,     // stay silent
};

// A set of zones loaded at startup and never changed afterwards.
// findZone() is a table lookup, safe for the audio thread.
class Instrument {
public:
    Instrument(std::string id, std::string name);

    void addZone(Zone zone);
    // Builds the note x velocity lookup table. Call once, after the last addZone().
    void finalize(FillMode fill);

    // Zone to play for this note and velocity (1..127), or nullptr.
    const Zone* findZone(int note, int velocity) const;

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    std::size_t zoneCount() const { return zones_.size(); }
    std::size_t memoryBytes() const;
    bool hasLoops() const;
    int dualMonoCount() const;  // stereo files loaded as mono (identical channels)

private:
    std::string id_;
    std::string name_;
    std::vector<std::unique_ptr<Zone>> zones_;  // stable addresses
    std::vector<const Zone*> map_;              // 128 notes x 128 velocities
};

} // namespace mbx
