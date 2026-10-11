// programs.json / presets.json parsing and preset switching in the engine.

#include "check.h"
#include "config/ProgramConfig.h"
#include "core/Engine.h"
#include "sampler/Instrument.h"
#include "sampler/InstrumentStore.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace mbx;
using nlohmann::json;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::unique_ptr<Instrument> sineInstrument(const std::string& id, double freq) {
    Zone z;
    z.channels = 1;
    z.sampleRate = kRate;
    z.frames = static_cast<std::uint32_t>(kRate);  // 1 s
    z.rootNote = 60;
    z.data.resize(z.frames);
    for (std::uint32_t i = 0; i < z.frames; ++i)
        z.data[i] = static_cast<std::int16_t>(std::lround(0.5 * 32767.0 * std::sin(2 * kPi * freq * i / kRate)));
    auto inst = std::make_unique<Instrument>(id, id);
    inst->addZone(std::move(z));
    inst->finalize(FillMode::Nearest);
    return inst;
}

bool hasWarning(const std::vector<std::string>& w, const std::string& text) {
    return std::any_of(w.begin(), w.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
}

std::vector<float> run(Engine& engine, double seconds) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> stereo(static_cast<std::size_t>(frames) * 2);
    for (int pos = 0; pos < frames; pos += 128) engine.render(stereo.data() + 2 * pos, std::min(128, frames - pos));
    std::vector<float> left(frames);
    for (int i = 0; i < frames; ++i) left[i] = stereo[2 * i];
    return left;
}

double frequency(const std::vector<float>& x, std::size_t from, std::size_t to) {
    int crossings = 0;
    std::size_t first = 0, last = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            if (crossings == 0) first = i;
            last = i;
            ++crossings;
        }
    }
    return crossings < 2 ? 0.0 : (crossings - 1) * kRate / static_cast<double>(last - first);
}

float peak(const std::vector<float>& x, std::size_t from, std::size_t to) {
    float p = 0.0f;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

const char* kPrograms = R"({
  "version": 1,
  "programs": [
    {
      "id": "low", "name": "Low sine", "output": "B", "gain_db": -6,
      "loop": "on",
      "adsr": { "attack_ms": 5, "release_ms": 300 },
      "velocity": { "mode": "on", "levels": 8, "curve": "exp" },
      "layers": [
        { "instrument": "SINE250", "keys": ["C1", "C4"], "transpose": 12, "gain_db": -3,
          "adsr": { "release_ms": 50 }, "loop": "off", "velocity": { "mode": "off" } },
        { "instrument": "MISSING" }
      ],
      "fx": [ { "type": "leslie", "params": { "speed": "slow" } }, { "type": "flanger" } ]
    },
    { "id": "high", "layers": [ { "instrument": "SINE400" } ] },
    { "id": "split", "layers": [
        { "instrument": "SINE250", "keys": [0, 59] },
        { "instrument": "SINE400", "keys": [60, 127] } ] },
    { "id": "stack", "layers": [ { "instrument": "SINE250" }, { "instrument": "SINE250" } ] },
    { "id": "ghost", "layers": [ { "instrument": "MISSING" } ] },
    { "id": "badout", "output": "C", "layers": [ { "instrument": "SINE250" } ] },
    { "id": "typo", "adsr": { "atack_ms": 5 }, "layers": [ { "instrument": "SINE250" } ] },
    { "id": "high", "layers": [ { "instrument": "SINE250" } ] },
    { "id": "layered", "layers": [
        { "instrument": "SINE250", "keys": ["C-2", "B2"] },
        { "instrument": "SINE400", "keys": ["C3", "G8"] },
        { "instrument": "SINE250", "keys": ["C3", "G8"] } ] },
    { "id": "four", "layers": [ { "instrument": "SINE250" }, { "instrument": "SINE400" },
                               { "instrument": "SINE250" }, { "instrument": "SINE400" } ] }
  ]
})";

} // namespace

int main() {
    // --- A velocity range with no zone falls back to the nearest velocity layer.
    {
        auto inst = std::make_unique<Instrument>("V", "V");
        for (int root : {48, 72}) {
            Zone z;
            z.channels = 1;
            z.sampleRate = kRate;
            z.frames = 16;
            z.rootNote = root;
            z.loVel = 45;  // only a middle layer, 45..84
            z.hiVel = 84;
            z.data.assign(16, 0);
            inst->addZone(std::move(z));
        }
        inst->finalize(FillMode::Nearest);
        CHECK(inst->findZone(50, 20) && inst->findZone(50, 20)->rootNote == 48);   // below: nearest layer, nearest note
        CHECK(inst->findZone(70, 120) && inst->findZone(70, 120)->rootNote == 72); // above
        CHECK(inst->findZone(60, 60) != nullptr);
    }

    InstrumentStore store;
    store.add(sineInstrument("SINE250", 250.0));
    store.add(sineInstrument("SINE400", 400.0));

    ProgramBank bank;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(loadPrograms(json::parse(kPrograms), store, bank, warnings, error));

    // --- Program fields and layer overrides ----------------------------------
    const Program* low = bank.find("low");
    CHECK(low != nullptr);
    if (low) {
        CHECK(low->name == "Low sine");
        CHECK(low->output == Output::B);
        CHECK_NEAR(low->gain, std::pow(10.0, -6.0 / 20.0), 1e-4);
        CHECK(low->layers.size() == 1);  // the MISSING layer is skipped
        const Layer& l = low->layers[0];
        CHECK(l.keyLo == 36 && l.keyHi == 72);
        CHECK(l.transpose == 12);
        CHECK_NEAR(l.gain, std::pow(10.0, -3.0 / 20.0), 1e-4);
        CHECK_NEAR(l.adsr.attackMs, 5.0, 1e-6);    // inherited
        CHECK_NEAR(l.adsr.releaseMs, 50.0, 1e-6);  // overridden
        CHECK(!l.loop);                            // overridden
        CHECK(!l.velocity.enabled);                // overridden
        CHECK(low->fx.size() == 2 && low->fx[0].type == "leslie");
        CHECK(low->fx[0].paramsJson.find("slow") != std::string::npos);
    }
    const Program* high = bank.find("high");
    CHECK(high && high->name == "high" && high->output == Output::A && high->layers[0].instrument->id() == "SINE400");

    CHECK(hasWarning(warnings, "program low layer 2: instrument MISSING is not loaded"));
    CHECK(hasWarning(warnings, "program ghost: no usable layer"));
    CHECK(hasWarning(warnings, "program badout: \"output\""));
    CHECK(hasWarning(warnings, "unknown key \"atack_ms\""));
    CHECK(hasWarning(warnings, "unknown effect type \"flanger\""));
    CHECK(hasWarning(warnings, "program high: duplicate id"));
    CHECK(bank.find("ghost") == nullptr && bank.find("badout") == nullptr);
    CHECK(bank.find("typo") != nullptr);  // a typo only warns
    CHECK(bank.programs.size() == 7);     // low, high, split, stack, typo, layered, four

    // Only the first max_layers (default 3) layers are used.
    const Program* four = bank.find("four");
    CHECK(four && four->layers.size() == 3);
    CHECK(hasWarning(warnings, "program four: 4 layers, only the first 3 are used"));

    // --- Presets ----------------------------------------------------------------
    warnings.clear();
    CHECK(loadPresets(json::parse(R"({
        "version": 1, "name": "Test", "startup": 4,
        "presets": { "1": "low", "2": "high", "3": "ghost", "5": "split", "6": "stack", "7": "layered",
                     "24": "high", "128": "split", "129": "low", "x": null }
    })"),
                      bank, warnings, error));
    CHECK(bank.setlistName == "Test");
    CHECK(bank.presets[0] == low && bank.presets[1] == high);
    CHECK(bank.presets[2] == nullptr);  // program not available
    CHECK(hasWarning(warnings, "preset 3: program ghost not available"));
    CHECK(bank.presets[23] == high && bank.presets[127] != nullptr);  // any number up to 128
    CHECK(hasWarning(warnings, "\"129\" is not a preset number"));
    CHECK(hasWarning(warnings, "\"startup\" is not a defined preset"));
    CHECK(bank.startupSlot == 1);  // startup 4 is empty: first filled one

    // --- Preset switching in the engine -------------------------------------------
    {
        Engine engine;
        engine.prepare(kRate);
        engine.setMasterGainDb(0.0f);
        engine.setPresets(&bank.presets);
        CHECK(!engine.selectPresetNow(4));
        CHECK(engine.selectPresetNow(2));  // high: 400 Hz
        CHECK(engine.currentSlot() == 2);
        const int src = engine.registerSource();

        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        auto x = run(engine, 0.2);
        CHECK_NEAR(frequency(x, 2400, x.size()), 400.0, 1.5);

        // Program Change to the slot already playing, or to an empty slot: ignored.
        engine.post(src, EngineEvent::fromMidi({0xC0, 1, 0}));  // preset 2
        engine.post(src, EngineEvent::fromMidi({0xC0, 3, 0}));  // preset 4, empty
        engine.post(src, EngineEvent::fromMidi({0xC0, 40, 0}));  // preset 41, not defined
        x = run(engine, 0.1);
        CHECK(peak(x, 0, x.size()) > 0.4f);
        CHECK(engine.currentSlot() == 2);

        // The last Program Change, 127, selects preset 128; then back to 2.
        engine.post(src, EngineEvent::fromMidi({0xC0, 127, 0}));
        run(engine, 0.02);
        CHECK(engine.currentSlot() == 128);
        engine.post(src, EngineEvent::fromMidi({0xC0, 1, 0}));
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        run(engine, 0.05);
        CHECK(engine.currentSlot() == 2);

        // Switch to preset 5 (split): the held note is cut within a few ms.
        engine.post(src, EngineEvent::fromMidi({0xC0, 4, 0}));
        x = run(engine, 0.1);
        CHECK(engine.currentSlot() == 5);
        CHECK(peak(x, static_cast<std::size_t>(0.012 * kRate), x.size()) < 1e-3f);
        CHECK(engine.activeVoices() == 0);

        // Split: below C3 the 250 Hz layer, from C3 up the 400 Hz layer.
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        x = run(engine, 0.2);
        CHECK_NEAR(frequency(x, 2400, x.size()), 400.0, 1.5);
        engine.post(src, EngineEvent::fromMidi({0x80, 60, 0}));
        run(engine, 0.3);
        engine.post(src, EngineEvent::fromMidi({0x90, 59, 127}));
        x = run(engine, 0.2);
        CHECK_NEAR(frequency(x, 2400, x.size()), 250.0 * std::pow(2.0, -1.0 / 12.0), 1.5);
        // Stack: two layers of 0.5 sum to 1.0 (0 dBFS); master at -6 dB keeps the
        // sum below the limiter threshold so it can be measured.
        engine.post(src, EngineEvent::masterGainDb(-6.0f));
        engine.post(src, EngineEvent::selectPreset(6));
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        x = run(engine, 0.2);
        CHECK_NEAR(peak(x, 2400, x.size()), 0.5, 0.01);
        CHECK(engine.activeVoices() == 2);

        // Layered program: 3 layers, split at B2 / C3. A bass note starts
        // one voice, a note from C3 up starts two (two layers).
        engine.post(src, EngineEvent::selectPreset(7));
        engine.post(src, EngineEvent::fromMidi({0x90, 48, 127}));
        run(engine, 0.02);
        CHECK(engine.activeVoices() == 1);
        engine.post(src, EngineEvent::fromMidi({0x90, 64, 127}));
        engine.post(src, EngineEvent::fromMidi({0x90, 67, 127}));
        run(engine, 0.02);
        CHECK(engine.activeVoices() == 5);
    }

    // --- File errors ------------------------------------------------------------
    {
        ProgramBank b;
        std::vector<std::string> w;
        std::string err;
        CHECK(!loadPrograms(json::parse(R"({"version": 1})"), store, b, w, err));
        CHECK(err.find("programs") != std::string::npos);
        CHECK(!loadProgramsFile("does/not/exist.json", store, b, w, err));
    }

    return finish("test_programs");
}
