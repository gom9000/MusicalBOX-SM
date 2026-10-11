// Program output A / B / AB -> the two DAC mute pins: muted at start, a
// startup wait, the pins follow the program after the audio-buffer delay.

#include "check.h"
#include "app/BankManager.h"
#include "app/OutputRouter.h"
#include "core/Engine.h"
#include "sampler/Instrument.h"
#include "sampler/InstrumentStore.h"

#include <memory>
#include <vector>

using namespace mbx;
using nlohmann::json;
using Clock = OutputRouter::Clock;
using std::chrono::milliseconds;

int main() {
    InstrumentStore store;
    {
        Zone z;
        z.channels = 1;
        z.sampleRate = 48000;
        z.frames = 4800;
        z.rootNote = 60;
        z.data.assign(z.frames, 0);
        auto inst = std::make_unique<Instrument>("S", "S");
        inst->addZone(std::move(z));
        inst->finalize(FillMode::Nearest);
        store.add(std::move(inst));
    }
    Engine engine;
    BankManager banks(engine, store, 3);
    std::vector<std::string> warnings;
    std::string error;
    auto bank = banks.build(json::parse(R"({"version": 1, "programs": [
        {"id": "a", "name": "A", "layers": [{"instrument": "S"}]},
        {"id": "b", "name": "B", "output": "B", "layers": [{"instrument": "S"}]},
        {"id": "ab", "name": "AB", "output": "AB", "layers": [{"instrument": "S"}]}]})"),
                            json{{"version", 1}, {"startup", 1}, {"presets", {{"1", "a"}, {"2", "b"}, {"3", "ab"}}}},
                            warnings, error);
    CHECK(bank != nullptr);
    if (!bank) return finish("test_output_router");
    banks.install(bank);
    engine.prepare(48000);
    CHECK(engine.currentOutput() == -1);
    CHECK(engine.selectPresetNow(1));
    CHECK(engine.currentOutput() == 0);
    const int src = engine.registerSource();
    std::vector<float> buf(2 * 128);

    std::vector<int> calls;  // every setPins: bit 0 A, bit 1 B
    OutputRouter router(engine, [&](bool a, bool b) { calls.push_back((a ? 1 : 0) | (b ? 2 : 0)); });
    CHECK(calls.size() == 1 && calls[0] == 0);  // muted at construction

    // Startup: nothing before kStartupMs, then DAC A.
    const auto t0 = Clock::now();
    router.begin(8.0, t0);
    router.poll(t0);
    router.poll(t0 + milliseconds(100));
    CHECK(!router.pinA() && !router.pinB());
    router.poll(t0 + milliseconds(OutputRouter::kStartupMs));
    CHECK(router.pinA() && !router.pinB());

    // Program on B: the pins change after the buffer delay (8 ms), not before.
    auto t = t0 + milliseconds(1000);
    engine.post(src, EngineEvent::selectPreset(2));
    engine.render(buf.data(), 128);
    CHECK(engine.currentOutput() == 1);
    router.poll(t);
    CHECK(router.pinA() && !router.pinB());
    router.poll(t + milliseconds(5));
    CHECK(router.pinA());
    router.poll(t + milliseconds(8));
    CHECK(!router.pinA() && router.pinB());

    // AB, then back to A before the delay ran out: only A is ever applied.
    t += milliseconds(100);
    engine.post(src, EngineEvent::selectPreset(3));
    engine.render(buf.data(), 128);
    router.poll(t);
    router.poll(t + milliseconds(9));
    CHECK(router.pinA() && router.pinB());
    const std::size_t before = calls.size();
    t += milliseconds(100);
    engine.post(src, EngineEvent::selectPreset(1));
    engine.render(buf.data(), 128);
    router.poll(t);
    engine.post(src, EngineEvent::selectPreset(2));
    engine.render(buf.data(), 128);
    router.poll(t + milliseconds(4));  // B now: the timer starts again
    router.poll(t + milliseconds(10));
    CHECK(router.pinA() && router.pinB());  // still AB: B is due at +12 ms
    router.poll(t + milliseconds(13));
    CHECK(!router.pinA() && router.pinB());
    CHECK(calls.size() == before + 1);  // A alone was never applied

    // Stop: both muted.
    router.stop();
    CHECK(calls.back() == 0 && !router.pinA() && !router.pinB());

    CHECK(OutputRouter::pinsFor(-1) == 0 && OutputRouter::pinsFor(0) == 1 && OutputRouter::pinsFor(1) == 2 &&
          OutputRouter::pinsFor(2) == 3);
    return finish("test_output_router");
}
