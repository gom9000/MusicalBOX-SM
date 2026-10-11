#include "app/BankManager.h"

#include "core/Engine.h"
#include "fx/FxChain.h"

#include <algorithm>

namespace mbx {

namespace {
constexpr auto kKeepRetired = std::chrono::milliseconds(500);  // much longer than kFxFadeMs
}

BankManager::BankManager(Engine& engine, const InstrumentStore& store, int maxLayers)
    : engine_(engine), store_(store), maxLayers_(maxLayers) {}

std::shared_ptr<ProgramBank> BankManager::build(const nlohmann::json& programs, const nlohmann::json& presets,
                                                std::vector<std::string>& warnings, std::string& error) const {
    auto bank = std::make_shared<ProgramBank>();
    if (!loadPrograms(programs, store_, *bank, warnings, error, maxLayers_)) return nullptr;
    if (!loadPresets(presets, *bank, warnings, error)) return nullptr;
    if (bank->startupSlot == 0) {
        error = "no preset is usable";
        return nullptr;
    }
    return bank;
}

void BankManager::install(std::shared_ptr<ProgramBank> bank) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(bank);
    engine_.setPresets(&current_->presets);
}

bool BankManager::swap(std::shared_ptr<ProgramBank> bank, int source) {
    // Effects get their buffers here, not in the audio thread.
    for (const auto& program : bank->programs)
        if (program->fxChain && !program->fxChain->prepared()) program->fxChain->prepare(engine_.sampleRate());

    std::lock_guard<std::mutex> lock(mutex_);
    const int generation = generation_ + 1;
    if (!engine_.post(source, EngineEvent::swapPresets(&bank->presets, generation))) return false;
    generation_ = generation;
    if (current_) retired_.push_back({std::move(current_), generation, {}});
    current_ = std::move(bank);
    return true;
}

std::shared_ptr<const ProgramBank> BankManager::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void BankManager::collect() {
    const int applied = engine_.appliedGeneration();
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<ProgramBank>> freeing;  // destroyed outside the lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::remove_if(retired_.begin(), retired_.end(), [&](Retired& r) {
            if (applied < r.generation) return false;
            if (r.applied == std::chrono::steady_clock::time_point{}) r.applied = now;
            if (now - r.applied < kKeepRetired) return false;
            freeing.push_back(std::move(r.bank));
            return true;
        });
        retired_.erase(it, retired_.end());
    }
}

} // namespace mbx
