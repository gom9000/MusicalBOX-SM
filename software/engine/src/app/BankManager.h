#pragma once

#include "config/ProgramConfig.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mbx {

class Engine;
class InstrumentStore;

// Owns the programs and presets the engine plays, and replaces them while it
// runs (web editor: preview, save, reload) without stopping the sound.
//
// A new bank is built and its effects prepared outside the audio thread, then
// handed to the engine with a SwapPresets event. The old bank stays alive
// until the audio thread has taken the new one and the cross-fade of the
// effects is over; collect() frees it after that.
//
// Thread safety: every method may be called from any non-audio thread.
class BankManager {
public:
    BankManager(Engine& engine, const InstrumentStore& store, int maxLayers);

    // Builds a bank from the two JSON documents (programs.json, presets.json).
    // nullptr when they are unusable (error set); warnings as --check prints them.
    std::shared_ptr<ProgramBank> build(const nlohmann::json& programs, const nlohmann::json& presets,
                                       std::vector<std::string>& warnings, std::string& error) const;

    // First bank, before the audio starts: given to the engine directly.
    void install(std::shared_ptr<ProgramBank> bank);
    // Later banks: posted to the engine through `source` (the caller's).
    // False if the engine queue is full (try again).
    bool swap(std::shared_ptr<ProgramBank> bank, int source);

    std::shared_ptr<const ProgramBank> current() const;
    // Frees the banks the engine no longer uses. Call now and then (main loop).
    void collect();

private:
    struct Retired {
        std::shared_ptr<ProgramBank> bank;
        int generation;  // the engine stops using it when it applies this generation...
        std::chrono::steady_clock::time_point applied{};  // ...and the cross-fade is over soon after
    };

    Engine& engine_;
    const InstrumentStore& store_;
    int maxLayers_;
    mutable std::mutex mutex_;
    std::shared_ptr<ProgramBank> current_;
    int generation_ = 0;
    std::vector<Retired> retired_;
};

} // namespace mbx
