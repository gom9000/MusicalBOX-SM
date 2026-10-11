#pragma once

#include "core/Constants.h"
#include "sampler/Program.h"

#include "nlohmann/json.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mbx {

class InstrumentStore;

// Every program of programs.json, and the preset table of presets.json.
// Built once at startup; the engine only reads it.
struct ProgramBank {
    std::vector<std::unique_ptr<Program>> programs;
    PresetTable presets{};    // nullptr = empty slot
    std::string setlistName;
    int startupSlot = 0;      // 0 = no usable preset

    const Program* find(const std::string& id) const;
    int fxDefinitions() const;
};

// programs.json. Problems inside one program skip that layer or that program
// (with a warning); false only when the whole file is unusable. Layers after
// the first `maxLayers` of a program are ignored with a warning.
bool loadPrograms(const nlohmann::json& root, const InstrumentStore& store, ProgramBank& bank,
                  std::vector<std::string>& warnings, std::string& error, int maxLayers = kDefaultMaxLayers);
bool loadProgramsFile(const std::filesystem::path& file, const InstrumentStore& store, ProgramBank& bank,
                      std::vector<std::string>& warnings, std::string& error, int maxLayers = kDefaultMaxLayers);

// presets.json, to be read after the programs.
bool loadPresets(const nlohmann::json& root, ProgramBank& bank, std::vector<std::string>& warnings,
                 std::string& error);
bool loadPresetsFile(const std::filesystem::path& file, ProgramBank& bank, std::vector<std::string>& warnings,
                     std::string& error);

} // namespace mbx
