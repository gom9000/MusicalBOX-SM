// The organ motor ("motor" in fx): the switch slows every voice of the
// program down to silence and brings it back, pitch and level together, each
// move starting from the current speed. And the pitch bend, which uses the
// same path to the voices.

#include "check.h"
#include "app/BankManager.h"
#include "core/Engine.h"
#include "sampler/Instrument.h"
#include "sampler/InstrumentStore.h"

#include <cmath>
#include <memory>
#include <vector>

using namespace mbx;
using nlohmann::json;

namespace {

constexpr double kRate = 48000.0;

// 250 Hz sine, 2 s, looped over whole periods (192 samples).
std::unique_ptr<Instrument> organ() {
    Zone z;
    z.channels = 1;
    z.sampleRate = kRate;
    z.frames = 96000;
    z.rootNote = 60;
    z.hasLoop = true;
    z.loopStart = 4800;
    z.loopEnd = 4800 + 192 * 400 - 1;
    z.data.resize(z.frames);
    for (std::uint32_t i = 0; i < z.frames; ++i)
        z.data[i] = static_cast<std::int16_t>(std::lround(0.4 * 32767.0 * std::sin(2 * 3.14159265358979 * 250.0 * i / kRate)));
    auto inst = std::make_unique<Instrument>("ORGAN", "ORGAN");
    inst->addZone(std::move(z));
    inst->finalize(FillMode::Nearest);
    return inst;
}

std::vector<float> run(Engine& engine, double seconds) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> out(2 * static_cast<std::size_t>(frames));
    for (int pos = 0; pos < frames; pos += 128) engine.render(out.data() + 2 * pos, std::min(128, frames - pos));
    return out;
}

// Frequency (rising zero crossings) and RMS of the left channel over the last `seconds` of x.
struct Measure {
    double hz, rms;
};
Measure measure(const std::vector<float>& x, double seconds) {
    const std::size_t n = x.size() / 2, from = n - static_cast<std::size_t>(seconds * kRate);
    int crossings = 0;
    double first = -1, last = -1, sum = 0;
    for (std::size_t i = from + 1; i < n; ++i) {
        const float a = x[2 * (i - 1)], b = x[2 * i];
        sum += double(b) * b;
        if (a < 0 && b >= 0) {
            const double t = (i - 1) + a / (a - b);
            if (first < 0) first = t;
            last = t;
            ++crossings;
        }
    }
    const double hz = crossings > 1 ? (crossings - 1) * kRate / (last - first) : 0.0;
    return {hz, std::sqrt(sum / double(n - from))};
}

} // namespace

int main() {
    InstrumentStore store;
    store.add(organ());
    Engine engine;
    engine.setMasterGainDb(0.0f);
    BankManager banks(engine, store, 3);
    std::vector<std::string> warnings;
    std::string error;
    auto bank = banks.build(json::parse(R"({"version": 1, "programs": [
        {"id": "organ", "name": "Organ", "loop": "on", "adsr": {"attack_ms": 1, "release_ms": 50},
         "layers": [{"instrument": "ORGAN"}],
         "fx": [{"type": "motor", "params": {"stop_s": 1, "start_s": 0.5}, "switch": ["state", "on", "off"]}]},
        {"id": "plain", "name": "Plain", "loop": "on", "layers": [{"instrument": "ORGAN"}],
         "fx": [{"type": "motor"}, {"type": "motor"}]}
    ]})"),
                            json{{"version", 1}, {"startup", 1}, {"presets", {{"1", "organ"}, {"2", "plain"}}}},
                            warnings, error);
    CHECK(bank != nullptr);
    if (!bank) return finish("test_motor");
    bool oneMotor = false;
    for (const auto& w : warnings) oneMotor |= w.find("only one motor per program") != std::string::npos;
    CHECK(oneMotor);
    banks.install(bank);
    engine.prepare(kRate);
    CHECK(engine.selectPresetNow(1));
    const int src = engine.registerSource();

    // Running: 250 Hz.
    engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
    auto x = run(engine, 0.3);
    const Measure full = measure(x, 0.1);
    CHECK_NEAR(full.hz, 250.0, 1.0);

    // Motor off: halfway through stop_s (0.5 s) the speed is 0.5 -> about 125 Hz, half the level.
    engine.post(src, EngineEvent::fxSwitch());
    x = run(engine, 0.5);
    Measure half = measure(x, 0.02);
    CHECK_NEAR(half.hz / 250.0, 0.5, 0.05);
    CHECK_NEAR(half.rms / full.rms, 0.5, 0.06);
    // Stopped: silence, the note is still held.
    x = run(engine, 0.7);
    CHECK(measure(x, 0.1).rms < 1e-4);
    CHECK(engine.activeVoices() == 1);

    // Motor on: from 0 up in start_s (0.5 s); after 0.25 s about half speed.
    engine.post(src, EngineEvent::fxSwitch());
    x = run(engine, 0.25);
    half = measure(x, 0.02);
    CHECK_NEAR(half.hz / 250.0, 0.5, 0.06);
    x = run(engine, 0.4);
    CHECK_NEAR(measure(x, 0.1).hz, 250.0, 1.0);
    CHECK_NEAR(measure(x, 0.1).rms / full.rms, 1.0, 0.02);

    // A partial stop (0.3 s -> speed 0.7), then on: back to full in 0.15 s, not 0.5 s.
    engine.post(src, EngineEvent::fxSwitch());
    run(engine, 0.3);
    engine.post(src, EngineEvent::fxSwitch());
    x = run(engine, 0.2);
    CHECK_NEAR(measure(x, 0.04).hz, 250.0, 2.0);

    // Stop, then another program: its motor runs, and back on the organ the motor is on again.
    engine.post(src, EngineEvent::fxSwitch());
    run(engine, 0.5);
    engine.post(src, EngineEvent::selectPreset(2));
    engine.post(src, EngineEvent::selectPreset(1));
    engine.post(src, EngineEvent::fromMidi({0x90, 64, 127}));
    x = run(engine, 0.2);
    CHECK_NEAR(measure(x, 0.1).hz, 250.0 * std::pow(2.0, 4 / 12.0), 1.5);
    engine.post(src, EngineEvent::fromMidi({0x80, 64, 0}));
    run(engine, 0.2);

    // Pitch bend: full up = +2 semitones (default range), centre = back.
    engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
    engine.post(src, EngineEvent::fromMidi({0xE0, 0x7F, 0x7F}));
    x = run(engine, 0.2);
    CHECK_NEAR(measure(x, 0.1).hz, 250.0 * std::pow(2.0, (8191.0 / 8192.0) * 2 / 12.0), 1.0);
    engine.post(src, EngineEvent::fromMidi({0xE0, 0x00, 0x40}));
    x = run(engine, 0.2);
    CHECK_NEAR(measure(x, 0.1).hz, 250.0, 1.0);
    engine.setPitchBendRange(0.0f);  // ignored
    engine.post(src, EngineEvent::fromMidi({0xE0, 0x00, 0x00}));
    x = run(engine, 0.2);
    CHECK_NEAR(measure(x, 0.1).hz, 250.0, 1.0);
    return finish("test_motor");
}
