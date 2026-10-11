// engine.json: defaults, values, invalid values, missing file.

#include "check.h"
#include "config/EngineConfig.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

using namespace mbx;

static bool hasWarning(const std::vector<std::string>& w, const std::string& text) {
    return std::any_of(w.begin(), w.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
}

static std::filesystem::path writeTemp(const char* name, const char* text) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << text;
    return path;
}

int main() {
    std::vector<std::string> warnings;
    std::string error;
    bool found = true;

    // Missing file: defaults, not an error.
    EngineSettings d;
    CHECK(loadEngineSettings("no/such/engine.json", d, found, warnings, error));
    CHECK(!found);
    CHECK(d.polyphony == 64 && d.maxLayers == 3 && d.masterVolumeDb == -6.0f && d.midiChannel == 0);
    CHECK(d.sampleRate == 48000 && d.periodFrames == 128 && d.periods == 3 && d.ramBudgetPercent == 70);

    // Values.
    auto good = writeTemp("mbx_engine_good.json", R"({
        "version": 1,
        // comments are allowed
        "audio": { "device": "hifiberry", "sample_rate": 44100, "period_frames": 64, "periods": 2 },
        "master_volume_db": -12.5, "polyphony": 96, "max_layers": 2,
        "ram_budget_percent": 60, "midi_channel": 3, "_note": "ignored"
    })");
    EngineSettings s;
    CHECK(loadEngineSettings(good, s, found, warnings, error));
    CHECK(found && warnings.empty());
    CHECK(s.audioDevice == "hifiberry" && s.sampleRate == 44100 && s.periodFrames == 64 && s.periods == 2);
    CHECK_NEAR(s.masterVolumeDb, -12.5, 1e-6);
    CHECK(s.polyphony == 96 && s.maxLayers == 2 && s.ramBudgetPercent == 60 && s.midiChannel == 3);

    // Invalid values: warning, default kept.
    auto bad = writeTemp("mbx_engine_bad.json", R"({
        "version": 1, "polyphony": 1000, "max_layers": 2.5, "midi_channel": 17,
        "master_volume_db": "loud", "voices": 32
    })");
    EngineSettings b;
    warnings.clear();
    CHECK(loadEngineSettings(bad, b, found, warnings, error));
    CHECK(b.polyphony == 64 && b.maxLayers == 3 && b.midiChannel == 0 && b.masterVolumeDb == -6.0f);
    CHECK(hasWarning(warnings, "\"polyphony\" must be an integer between 8 and 256"));
    CHECK(hasWarning(warnings, "\"max_layers\""));
    CHECK(hasWarning(warnings, "\"midi_channel\""));
    CHECK(hasWarning(warnings, "\"master_volume_db\""));
    CHECK(hasWarning(warnings, "unknown key \"voices\""));

    // DAC mute pins.
    auto dac = writeTemp("mbx_engine_dac.json",
                         R"({"version": 1, "dac_mute": {"enabled": false, "a": 12, "b": 13, "chip": "gpiochip0"}})");
    EngineSettings dm;
    warnings.clear();
    CHECK(loadEngineSettings(dac, dm, found, warnings, error) && warnings.empty());
    CHECK(!dm.dacMute.enabled && dm.dacMute.a == 12 && dm.dacMute.b == 13 && dm.dacMute.chip == "gpiochip0");
    EngineSettings dd;
    CHECK(dd.dacMute.enabled && dd.dacMute.a == 16 && dd.dacMute.b == 17 && dd.dacMute.chip.empty());
    auto dacBad = writeTemp("mbx_engine_dac_bad.json", R"({"version": 1, "dac_mute": {"a": 20, "b": 20, "x": 1}})");
    EngineSettings db;
    warnings.clear();
    CHECK(loadEngineSettings(dacBad, db, found, warnings, error));
    CHECK(db.dacMute.a == 16 && db.dacMute.b == 17);
    CHECK(hasWarning(warnings, "must be different GPIOs") && hasWarning(warnings, "unknown key \"x\""));
    std::filesystem::remove(dac);
    std::filesystem::remove(dacBad);

    // Not JSON at all: error.
    auto broken = writeTemp("mbx_engine_broken.json", "{ polyphony: ");
    EngineSettings x;
    CHECK(!loadEngineSettings(broken, x, found, warnings, error));

    std::filesystem::remove(good);
    std::filesystem::remove(bad);
    std::filesystem::remove(broken);
    return finish("test_engine_config");
}
