// Live parameters: a value changed while playing (switch, MIDI controller,
// web editor, control socket) goes to the effect that is already playing,
// so tails and phases go on; only a different structure needs a new chain.

#include "check.h"
#include "app/BankManager.h"
#include "app/ControlApi.h"
#include "app/LiveStats.h"
#include "core/Engine.h"
#include "fx/Effects.h"
#include "fx/FxChain.h"
#include "sampler/Instrument.h"
#include "sampler/InstrumentStore.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace mbx;
using nlohmann::json;

namespace {

constexpr double kRate = 48000.0;

FxDef def(const char* type, const char* params, const char* sw = nullptr, const char* a = nullptr,
          const char* b = nullptr, const char* cc = nullptr) {
    FxDef d;
    d.type = type;
    d.paramsJson = params;
    if (sw) {
        d.switchParam = sw;
        d.switchA = a;
        d.switchB = b;
    }
    if (cc) d.ccJson = cc;
    return d;
}

// Runs a chain over `seconds`: a 440 Hz burst for the first `burst` seconds, then silence.
// Returns the left channel.
std::vector<float> play(FxChain& chain, double seconds, double burst, double startTime = 0.0) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> l(frames), r(frames);
    for (int i = 0; i < frames; ++i) {
        const double t = startTime + i / kRate;
        l[i] = r[i] = t < burst ? static_cast<float>(0.3 * std::sin(2 * 3.14159265358979 * 440.0 * t)) : 0.0f;
    }
    for (int pos = 0; pos < frames; pos += 128) chain.process(l.data() + pos, r.data() + pos, std::min(128, frames - pos));
    return l;
}

float peak(const std::vector<float>& x, int from = 0) {
    float p = 0.0f;
    for (std::size_t i = static_cast<std::size_t>(from); i < x.size(); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

float maxJump(const std::vector<float>& x) {
    float j = 0.0f;
    for (std::size_t i = 1; i < x.size(); ++i) j = std::max(j, std::fabs(x[i] - x[i - 1]));
    return j;
}

std::unique_ptr<Instrument> sine() {
    Zone z;
    z.channels = 1;
    z.sampleRate = kRate;
    z.frames = 48000;
    z.rootNote = 60;
    z.hasLoop = true;
    z.loopStart = 0;
    z.loopEnd = 48000 - 1;
    z.data.resize(z.frames);
    for (std::uint32_t i = 0; i < z.frames; ++i)
        z.data[i] = static_cast<std::int16_t>(std::lround(0.3 * 32767.0 * std::sin(2 * 3.14159265358979 * 250.0 * i / kRate)));
    auto inst = std::make_unique<Instrument>("SINE", "SINE");
    inst->addZone(std::move(z));
    inst->finalize(FillMode::Nearest);
    return inst;
}

std::vector<float> render(Engine& engine, double seconds) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> st(2 * static_cast<std::size_t>(frames)), left(frames);
    for (int pos = 0; pos < frames; pos += 128) engine.render(st.data() + 2 * pos, std::min(128, frames - pos));
    for (int i = 0; i < frames; ++i) left[i] = st[2 * static_cast<std::size_t>(i)];
    return left;
}

json programs(const char* echoParams, const char* echoExtra = "") {
    return json::parse(std::string(R"({"version": 1, "programs": [
        {"id": "echo", "name": "Echo", "adsr": {"attack_ms": 2, "release_ms": 5}, "layers": [{"instrument": "SINE"}],
         "fx": [{"type": "echo", "params": )") + echoParams + echoExtra + R"(}]},
        {"id": "plain", "name": "Plain", "layers": [{"instrument": "SINE"}]}]})");
}

} // namespace

int main() {
    std::vector<std::string> w;

    // --- A live value: the echo tail goes on, the value glides (no click).
    {
        auto chain = FxChain::build({def("echo", R"({"time_ms": 80, "feedback": 0.7, "mix": 0.5, "wow": 0})")}, "p", w);
        chain->prepare(kRate);
        play(*chain, 0.3, 0.2);
        chain->apply({0, TapeEcho::kMix, 1.0f, 0.0f}, false);  // mix 0.5 -> 1 while the tail rings
        auto tail = play(*chain, 0.3, 0.0);
        CHECK(peak(tail) > 0.05f);       // still echoing: nothing was cleared
        CHECK(maxJump(tail) < 0.1f);     // the change glides
        CHECK_NEAR(chain->effectAt(0).param(TapeEcho::kMix), 1.0f, 1e-6);
        // Back to the programmed value on restore (program selected again).
        chain->restore();
        CHECK_NEAR(chain->effectAt(0).param(TapeEcho::kMix), 0.5f, 1e-6);
    }

    // --- Switch on a live parameter: one instance, the tail survives the press.
    {
        w.clear();
        auto chain = FxChain::build({def("echo", R"({"time_ms": 80, "feedback": 0.7, "wow": 0})", "mix", "0.3", "0.8")},
                                    "p", w);
        CHECK(w.empty() && chain->hasSwitch());
        chain->prepare(kRate);
        play(*chain, 0.3, 0.2);
        chain->setSwitch(true, false);
        auto tail = play(*chain, 0.3, 0.0);
        CHECK(peak(tail, static_cast<int>(0.05 * kRate)) > 0.05f);  // the repeats go on after the press
        CHECK_NEAR(chain->effectAt(0).param(TapeEcho::kMix), 0.8f, 1e-6);
        chain->setSwitch(false, false);
        play(*chain, 0.1, 0.0);
        CHECK_NEAR(chain->effectAt(0).param(TapeEcho::kMix), 0.3f, 1e-6);
    }

    // --- Switch on a structural parameter (chorus mode): two instances, cross-fade.
    {
        w.clear();
        auto chain = FxChain::build({def("chorus", "{}", "mode", "\"ce2\"", "\"ensemble\"")}, "p", w);
        CHECK(w.empty() && chain->hasSwitch());
        chain->prepare(kRate);
        play(*chain, 0.2, 1.0);
        chain->setSwitch(true, false);
        auto x = play(*chain, 0.2, 1.0, 0.2);
        CHECK(maxJump(x) < 0.15f);
    }

    // --- MIDI controllers: numbers (full range, explicit range, log), choice, bypass.
    {
        w.clear();
        auto chain = FxChain::build(
            {def("echo", R"({"wow": 0})", nullptr, nullptr, nullptr,
                 R"({"mix": 21, "feedback": [22, 0.1, 0.5], "tone_hz": 23, "bypass": 24})"),
             def("leslie", "{}", nullptr, nullptr, nullptr, R"({"speed": 1})"),
             def("chorus", "{}", nullptr, nullptr, nullptr, R"({"mode": 25, "rate": 26, "mix": 64})")},
            "p", w);
        CHECK(w.size() == 3);  // mode not live, unknown "rate", controller 64
        chain->prepare(kRate);
        chain->midiControl(21, 127);
        chain->midiControl(22, 0);
        chain->midiControl(23, 127);
        chain->midiControl(1, 100);
        play(*chain, 0.2, 0.2);
        const Effect& echo = chain->effectAt(0);
        CHECK_NEAR(echo.param(TapeEcho::kMix), 1.0f, 1e-5);
        CHECK_NEAR(echo.param(TapeEcho::kFeedback), 0.1f, 1e-5);
        CHECK_NEAR(echo.param(TapeEcho::kTone), 12000.0f, 0.5);
        CHECK(chain->effectAt(1).param(Leslie::kSpeed) == 1.0f);  // fast
        chain->midiControl(23, 0);
        play(*chain, 0.2, 0.2);
        CHECK_NEAR(echo.param(TapeEcho::kTone), 500.0f, 0.1);       // log scale ends
        chain->midiControl(23, 64);
        play(*chain, 0.2, 0.2);
        CHECK(echo.param(TapeEcho::kTone) > 2200.0f && echo.param(TapeEcho::kTone) < 2600.0f);  // geometric middle
        // Bypass: to dry without a click, then back.
        chain->midiControl(24, 127);
        auto x = play(*chain, 0.2, 0.2);
        CHECK(maxJump(x) < 0.2f);
    }

    // --- Same structure: the chain can take the new values; other structure: rebuild.
    {
        w.clear();
        auto a = FxChain::build({def("echo", R"({"mix": 0.3})"), def("chorus", R"({"mode": "ensemble"})")}, "p", w);
        auto b = FxChain::build({def("echo", R"({"mix": 0.6, "time_ms": 200})"), def("chorus", R"({"mode": "ensemble"})")},
                                "p", w);
        auto c = FxChain::build({def("echo", R"({"mix": 0.6})"), def("chorus", R"({"mode": "ce2"})")}, "p", w);
        auto d = FxChain::build({def("echo", R"({"mix": 0.6})", nullptr, nullptr, nullptr, R"({"mix": 7})"),
                                 def("chorus", R"({"mode": "ensemble"})")},
                                "p", w);
        CHECK(a->sameStructure(*b));
        CHECK(!a->sameStructure(*c));  // chorus mode is not live
        CHECK(!a->sameStructure(*d));  // controllers are part of the structure
        const auto changes = a->changesTo(*b);
        CHECK(changes.size() == 2);    // mix and time
        auto e = FxChain::build({def("echo", "{}", "mix", "0.1", "0.5")}, "p", w);
        auto f = FxChain::build({def("echo", "{}", "mix", "0.2", "0.7")}, "p", w);
        CHECK(e->sameStructure(*f));
        const auto sw = e->changesTo(*f);
        CHECK(sw.size() == 1 && sw[0].param == FxChange::kSwitchValues && sw[0].value == 0.2f && sw[0].value2 == 0.7f);
    }

    // --- Engine and web editor: a new value keeps the chain playing (the tail goes on);
    //     a new structure cross-fades to a new chain as before.
    {
        InstrumentStore store;
        store.add(sine());
        Engine engine;
        engine.setMasterGainDb(0.0f);
        BankManager banks(engine, store, 3);
        std::string error;
        const json presets = {{"version", 1}, {"startup", 1}, {"presets", {{"1", "echo"}, {"2", "plain"}}}};
        auto bank = banks.build(programs(R"({"time_ms": 80, "feedback": 0.75, "mix": 0.6, "wow": 0})"), presets, w, error);
        CHECK(bank != nullptr);
        if (!bank) return finish("test_live");
        const FxChain* first = bank->programs[0]->fxChain.get();
        banks.install(bank);
        engine.prepare(kRate);
        CHECK(engine.selectPresetNow(1));
        const int src = engine.registerSource();
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        render(engine, 0.2);
        engine.post(src, EngineEvent::fromMidi({0x80, 60, 0}));
        render(engine, 0.05);

        // Edited value: same chain, the echoes go on.
        auto edited = banks.build(programs(R"({"time_ms": 80, "feedback": 0.75, "mix": 0.9, "wow": 0})"), presets, w, error);
        CHECK(edited && banks.swap(edited, src));
        CHECK(banks.current()->programs[0]->fxChain.get() == first);
        auto tail = render(engine, 0.3);
        CHECK(peak(tail, static_cast<int>(0.1 * kRate)) > 0.02f);
        CHECK_NEAR(first->effectAt(0).param(TapeEcho::kMix), 0.9f, 1e-5);

        // Controller from the control socket, then a program change and back: programmed value again.
        LiveStats stats;
        ControlApi api(engine, banks, store, "does-not-exist", stats, {}, 0, "test");
        json r = json::parse(api.handle(R"({"cmd": "param", "fx": 1, "param": "mix", "value": 0.2})"));
        CHECK(r["ok"] == true);
        render(engine, 0.2);
        CHECK_NEAR(first->effectAt(0).param(TapeEcho::kMix), 0.2f, 1e-5);
        CHECK(json::parse(api.handle(R"({"cmd": "param", "fx": 1, "param": "ping_pong", "value": true})"))["ok"] == false);
        CHECK(json::parse(api.handle(R"({"cmd": "param", "fx": 3, "param": "mix", "value": 0.2})"))["ok"] == false);
        engine.post(src, EngineEvent::selectPreset(2));
        render(engine, 0.1);
        engine.post(src, EngineEvent::selectPreset(1));
        render(engine, 0.1);
        CHECK_NEAR(first->effectAt(0).param(TapeEcho::kMix), 0.9f, 1e-5);

        // Structure changed (ping-pong is not live): a new chain.
        auto rebuilt = banks.build(programs(R"({"time_ms": 80, "feedback": 0.75, "mix": 0.9, "wow": 0, "ping_pong": true})"),
                                   presets, w, error);
        CHECK(rebuilt && banks.swap(rebuilt, src));
        render(engine, 0.1);
        CHECK(banks.current()->programs[0]->fxChain.get() != first);

        // MIDI controller through the engine.
        auto withCc = banks.build(programs(R"({"wow": 0})", R"(, "cc": {"mix": 20})"), presets, w, error);
        CHECK(withCc && banks.swap(withCc, src));
        render(engine, 0.1);
        engine.post(src, EngineEvent::fromMidi({0xB0, 20, 0}));
        render(engine, 0.2);
        CHECK_NEAR(banks.current()->programs[0]->fxChain->effectAt(0).param(TapeEcho::kMix), 0.0f, 1e-5);
    }

    return finish("test_live");
}
