#pragma once

#include "sampler/Instrument.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mbx {

// One entry of instruments.json.
struct InstrumentDef {
    std::string id;
    std::string name;
    std::string path;                 // relative to media_root
    std::string files;                // e.g. "{note}.wav", "{name}_{vel}.wav"
    std::vector<int> velocityLayers;  // minimum velocity of each {vel} index
    // Alternative: {vel} is a text label ("PP", "MF", "FF"...), each with its minimum
    // velocity. Sorted by velocity; when present, velocityLayers is filled to match.
    std::vector<std::pair<std::string, int>> velocityLabels;
    FillMode fill = FillMode::Nearest;
    int nameOffset = 0;               // semitones added to {name}: 0 for C3 = 60, -12 for C4 = 60
};

struct InstrumentsConfig {
    std::filesystem::path mediaRoot;  // already resolved
    std::vector<InstrumentDef> instruments;
};

// Reads instruments.json. A relative media_root is resolved against the
// folder that contains the file. Returns false (and sets `error`) only when
// the file cannot be used at all; problems with single entries are skipped
// and reported in `warnings`.
bool loadInstrumentsConfig(const std::filesystem::path& file, InstrumentsConfig& out,
                           std::vector<std::string>& warnings, std::string& error);

} // namespace mbx
