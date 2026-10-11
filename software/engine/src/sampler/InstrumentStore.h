#pragma once

#include "config/ConfigLoader.h"
#include "sampler/Instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace mbx {

// Owns every instrument, loaded once at startup.
class InstrumentStore {
public:
    // Instruments that would push the total over this are not loaded (0 = no limit).
    void setBudgetBytes(std::size_t bytes) { budget_ = bytes; }

    // Loads every instrument of the config. Missing folders, unreadable files
    // and over-budget instruments are skipped with a warning.
    void loadAll(const InstrumentsConfig& config, std::vector<std::string>& warnings);

    // Loads a single instrument; returns nullptr (with warnings) when nothing usable was found.
    static std::unique_ptr<Instrument> load(const InstrumentDef& def, const std::filesystem::path& mediaRoot,
                                            std::vector<std::string>& warnings);

    void add(std::unique_ptr<Instrument> instrument);
    const Instrument* find(const std::string& id) const;
    const std::vector<std::unique_ptr<Instrument>>& all() const { return instruments_; }
    std::size_t totalBytes() const { return total_; }

private:
    std::vector<std::unique_ptr<Instrument>> instruments_;
    std::size_t budget_ = 0;
    std::size_t total_ = 0;
};

} // namespace mbx
