// Effects: each type behaves as described, the chain is built from programs.json
// definitions, and a program change fades the old effects out.

#include "check.h"
#include "core/Engine.h"
#include "fx/Effects.h"
#include "fx/FxChain.h"
#include "sampler/Instrument.h"
#include "sampler/Program.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace mbx;
using nlohmann::json;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(int n) : l(n, 0.0f), r(n, 0.0f) {}
    int size() const { return static_cast<int>(l.size()); }
};

Stereo sine(double freq, double seconds, float amp = 0.5f) {
    Stereo s(static_cast<int>(seconds * kRate));
    for (int i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = amp * static_cast<float>(std::sin(2 * kPi * freq * i / kRate));
    return s;
}

Stereo noise(double seconds) {
    Stereo s(static_cast<int>(seconds * kRate));
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    for (int i = 0; i < s.size(); ++i) {
        s.l[i] = d(rng);
        s.r[i] = d(rng);
    }
    return s;
}

void run(Effect& e, Stereo& s) {
    for (int pos = 0; pos < s.size(); pos += 256) e.process(s.l.data() + pos, s.r.data() + pos, std::min(256, s.size() - pos));
}

double rms(const std::vector<float>& x, int from = 0, int to = -1) {
    if (to < 0) to = static_cast<int>(x.size());
    double sum = 0;
    for (int i = from; i < to; ++i) sum += double(x[i]) * x[i];
    return std::sqrt(sum / std::max(1, to - from));
}

float peak(const std::vector<float>& x, int from = 0, int to = -1) {
    if (to < 0) to = static_cast<int>(x.size());
    float p = 0;
    for (int i = from; i < to; ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

bool finite(const Stereo& s) {
    for (int i = 0; i < s.size(); ++i)
        if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
    return true;
}

std::unique_ptr<Effect> make(const std::string& type, const char* params, std::vector<std::string>& w) {
    auto e = createEffect(type, json::parse(params), "test", w);
    if (e) e->prepare(kRate);
    return e;
}

bool hasWarning(const std::vector<std::string>& w, const std::string& text) {
    return std::any_of(w.begin(), w.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
}

std::unique_ptr<Instrument> sineInstrument(double freq) {
    Zone z;
    z.channels = 1;
    z.sampleRate = kRate;
    z.frames = static_cast<std::uint32_t>(kRate);
    z.rootNote = 60;
    z.data.resize(z.frames);
    for (std::uint32_t i = 0; i < z.frames; ++i)
        z.data[i] = static_cast<std::int16_t>(std::lround(0.5 * 32767.0 * std::sin(2 * kPi * freq * i / kRate)));
    z.hasLoop = true;
    z.loopStart = 0;
    z.loopEnd = z.frames - 1;
    auto inst = std::make_unique<Instrument>("SINE", "SINE");
    inst->addZone(std::move(z));
    inst->finalize(FillMode::Nearest);
    return inst;
}

std::vector<float> render(Engine& engine, double seconds) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> st(2 * frames);
    for (int pos = 0; pos < frames; pos += 128) engine.render(st.data() + 2 * pos, std::min(128, frames - pos));
    std::vector<float> left(frames);
    for (int i = 0; i < frames; ++i) left[i] = st[2 * i];
    return left;
}

} // namespace

int main() {
    std::vector<std::string> w;

    // --- Every type with default parameters: finite, bounded, silent in -> silent out.
    for (const char* type : {"tone", "echo", "chorus", "phaser", "leslie", "tremolo", "fuzz", "width", "room"}) {
        auto e = make(type, "{}", w);
        CHECK(e != nullptr);
        if (!e) continue;
        Stereo s = noise(1.0);
        run(*e, s);
        CHECK(finite(s));
        CHECK(peak(s.l) < 4.0f && peak(s.r) < 4.0f);
        e->reset();
        Stereo z(48000);
        run(*e, z);
        CHECK(peak(z.l) == 0.0f && peak(z.r) == 0.0f);
    }
    CHECK(w.empty());

    // --- Tone: shelves do what they say.
    {
        auto e = make("tone", R"({"bass_db": 6, "treble_db": -6})", w);
        Stereo lo = sine(50.0, 1.0);
        run(*e, lo);
        CHECK_NEAR(rms(lo.l, 24000) / (0.5 / std::sqrt(2.0)), 2.0, 0.15);   // +6 dB
        e->reset();
        Stereo hi = sine(12000.0, 1.0);
        run(*e, hi);
        CHECK_NEAR(rms(hi.l, 24000) / (0.5 / std::sqrt(2.0)), 0.5, 0.06);   // -6 dB
        auto flat = make("tone", "{}", w);
        Stereo mid = sine(1000.0, 0.5);
        Stereo ref = mid;
        run(*flat, mid);
        CHECK(peak(mid.l) == peak(ref.l));
    }

    // --- Echo: one repeat after time_ms, repeats decay with feedback.
    {
        auto e = make("echo", R"({"time_ms": 100, "feedback": 0, "mix": 1, "wow": 0, "drive": 0, "tone_hz": 12000, "low_cut_hz": 20})", w);
        Stereo s(24000);
        s.l[0] = s.r[0] = 1.0f;
        run(*e, s);
        int at = 1;
        for (int i = 1; i < s.size(); ++i)
            if (std::fabs(s.l[i]) > std::fabs(s.l[at])) at = i;
        CHECK(std::abs(at - 4800) <= 3);
        CHECK(peak(s.l, 6000) < 0.05f);  // no second repeat with feedback 0

        auto fb = make("echo", R"({"time_ms": 100, "feedback": 0.5, "mix": 1, "wow": 0})", w);
        Stereo t(48000 * 3);
        t.l[0] = t.r[0] = 1.0f;
        run(*fb, t);
        const float first = peak(t.l, 4000, 6000), second = peak(t.l, 8800, 10800);
        CHECK(second < first && second > 0.2f * first);
        CHECK(peak(t.l, 48000 * 2) < 0.01f);  // decays

        auto pp = make("echo", R"({"time_ms": 100, "feedback": 0.5, "mix": 1, "wow": 0, "ping_pong": true})", w);
        Stereo u(24000);
        u.l[0] = u.r[0] = 1.0f;
        run(*pp, u);
        // (an impulse through the darkening filter: small peaks, but on the right side)
        CHECK(peak(u.l, 4000, 6000) > 0.05f && peak(u.r, 4000, 6000) < 1e-3f);  // first repeat left
        CHECK(peak(u.r, 8800, 10800) > 0.01f && peak(u.l, 8800, 10800) < 1e-3f); // second one right
    }

    // --- Chorus and ensemble: the signal moves, the level stays.
    for (const char* params : {R"({"mode": "ce2"})", R"({"mode": "ensemble"})"}) {
        auto e = make("chorus", params, w);
        Stereo s = sine(440.0, 1.0);
        Stereo ref = s;
        run(*e, s);
        double diff = 0;
        for (int i = 0; i < s.size(); ++i) diff += std::fabs(s.l[i] - ref.l[i]);
        CHECK(diff / s.size() > 0.01);
        CHECK(rms(s.l, 24000) > 0.5 * rms(ref.l) && rms(s.l, 24000) < 1.3 * rms(ref.l));
        double lr = 0;
        for (int i = 0; i < s.size(); ++i) lr += std::fabs(s.l[i] - s.r[i]);
        CHECK(lr / s.size() > 0.001);  // stereo
    }

    // --- Phaser: mix 0 is dry; with mix the sound changes over time.
    {
        auto dry = make("phaser", R"({"mix": 0})", w);
        Stereo s = noise(0.5);
        Stereo ref = s;
        run(*dry, s);
        CHECK(peak(s.l) == peak(ref.l));
        auto e = make("phaser", R"({"rate_hz": 1})", w);
        Stereo t = sine(800.0, 2.0);
        run(*e, t);
        // The notch passes over 800 Hz: the level of the 800 Hz tone varies.
        double lo = 1e9, hi = 0;
        for (int b = 0; b < 40; ++b) {
            const double r = rms(t.l, b * 2400, (b + 1) * 2400);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        CHECK(hi > 2.0 * lo);
    }

    // --- Leslie: two different microphones, sensible level, both speeds.
    for (const char* params : {R"({"speed": "slow"})", R"({"speed": "fast"})"}) {
        auto e = make("leslie", params, w);
        Stereo s = sine(1500.0, 1.0);
        Stereo ref = s;
        run(*e, s);
        double lr = 0;
        for (int i = 0; i < s.size(); ++i) lr += std::fabs(s.l[i] - s.r[i]);
        CHECK(lr / s.size() > 0.01);
        CHECK(rms(s.l, 24000) > 0.5 * rms(ref.l) && rms(s.l, 24000) < 1.5 * rms(ref.l));
    }

    // --- Leslie cabinet: the reflections make the two microphones far less
    // alike on a mono source, at about the same level.
    {
        auto corr = [](const Stereo& s, int from) {
            double lr = 0, ll = 0, rr = 0;
            for (int i = from; i < s.size(); ++i) lr += s.l[i] * s.r[i], ll += s.l[i] * s.l[i], rr += s.r[i] * s.r[i];
            return lr / std::sqrt(ll * rr);
        };
        // A chord-like mono tone: harmonics of 147 Hz up to about 3 kHz.
        Stereo mono(static_cast<int>(3.0 * kRate));
        for (int i = 0; i < mono.size(); ++i) {
            double v = 0;
            for (int h = 1; h <= 20; ++h) v += std::sin(2 * kPi * 146.8 * h * i / kRate + h * 1.3) / h;
            mono.l[i] = mono.r[i] = static_cast<float>(0.2 * v);
        }
        Stereo direct = mono, cab = mono;
        auto d = make("leslie", R"({"cabinet": 0, "top_hz": 20000})", w);
        auto c = make("leslie", R"({"cabinet": 1})", w);
        run(*d, direct);
        run(*c, cab);
        const double cd = corr(direct, 4800), cc = corr(cab, 4800);
        std::printf("leslie L/R correlation on a mono tone: direct %.2f, with cabinet %.2f\n", cd, cc);
        CHECK(cc < cd - 0.15);
        CHECK(rms(cab.l, 4800) > 0.5 * rms(mono.l) && rms(cab.l, 4800) < 1.5 * rms(mono.l));
    }

    // --- Room: dry sound untouched at level 0, a decorrelated tail that decays.
    {
        auto silent = make("room", R"({"level": 0})", w);
        Stereo s = sine(440.0, 0.5);
        Stereo ref = s;
        run(*silent, s);
        CHECK(s.l == ref.l && s.r == ref.r);

        auto e = make("room", R"({"time_s": 0.7})", w);
        Stereo burst(static_cast<int>(2.0 * kRate));
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        for (int i = 0; i < 4800; ++i) burst.l[i] = burst.r[i] = u(rng);
        run(*e, burst);
        CHECK(finite(burst));
        const double early = rms(burst.l, 4800, 9600), late = rms(burst.l, 4800 + 33600, 9600 + 33600);
        const double dropDb = 20.0 * std::log10(early / std::max(late, 1e-12));
        std::printf("room: %.1f dB down 0.7 s later (60 dB per time_s expected)\n", dropDb);
        CHECK(dropDb > 45.0 && dropDb < 75.0);
        double lr = 0, ll = 0, rr = 0;
        for (int i = 4800; i < 24000; ++i)
            lr += burst.l[i] * burst.r[i], ll += burst.l[i] * burst.l[i], rr += burst.r[i] * burst.r[i];
        CHECK(std::fabs(lr / std::sqrt(ll * rr)) < 0.3);  // tail: left and right uncorrelated
    }

    // --- Tremolo: depth 1 goes down to silence and back to full level.
    {
        auto e = make("tremolo", R"({"rate_hz": 4, "depth": 1})", w);
        Stereo s(48000);
        std::fill(s.l.begin(), s.l.end(), 1.0f);
        std::fill(s.r.begin(), s.r.end(), 1.0f);
        run(*e, s);
        CHECK(*std::min_element(s.l.begin(), s.l.end()) < 0.01f);
        CHECK(*std::max_element(s.l.begin(), s.l.end()) > 0.99f);
        auto pan = make("tremolo", R"({"rate_hz": 4, "depth": 1, "stereo": true})", w);
        Stereo p(12000);
        std::fill(p.l.begin(), p.l.end(), 1.0f);
        std::fill(p.r.begin(), p.r.end(), 1.0f);
        run(*pan, p);
        CHECK(std::fabs(p.l[3000] - p.r[3000]) > 0.9f);  // a quarter period in: opposite phases
    }
    // --- Width: a mono sound becomes stereo, L + R stays the dry sound (mono compatible).
    {
        auto e = make("width", R"({"amount": 1})", w);
        Stereo s = noise(0.5);
        for (int i = 0; i < s.size(); ++i) s.r[i] = s.l[i];  // mono input
        Stereo ref = s;
        run(*e, s);
        double lr = 0, monoErr = 0;
        for (int i = 0; i < s.size(); ++i) {
            lr += std::fabs(s.l[i] - s.r[i]);
            monoErr = std::max(monoErr, double(std::fabs(0.5f * (s.l[i] + s.r[i]) - ref.l[i])));
        }
        CHECK(lr / s.size() > 0.05);
        CHECK(monoErr < 1e-6);
        auto none = make("width", R"({"amount": 0})", w);
        Stereo t = sine(440.0, 0.2);
        Stereo tr = t;
        run(*none, t);
        CHECK(peak(t.l) == peak(tr.l) && peak(t.r) == peak(tr.r));
    }

    // --- Fuzz: blend 0 is dry; the octave adds the octave above; level close to the dry sound.
    {
        auto dry = make("fuzz", R"({"blend": 0})", w);
        Stereo s = sine(440.0, 0.5);
        Stereo ref = s;
        run(*dry, s);
        CHECK(peak(s.l) == peak(ref.l));

        auto goertzel = [](const std::vector<float>& x, double f, int from) {
            double s1 = 0, s2 = 0, c = 2 * std::cos(2 * kPi * f / kRate);
            for (std::size_t i = from; i < x.size(); ++i) {
                const double s0 = x[i] + c * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
        };
        auto plain = make("fuzz", R"({"octave": 0, "bias": 0, "tone_hz": 10000})", w);
        auto oct = make("fuzz", R"({"octave": 1, "bias": 0, "tone_hz": 10000})", w);
        Stereo a = sine(440.0, 0.5, 0.3), b = sine(440.0, 0.5, 0.3);
        run(*plain, a);
        run(*oct, b);
        CHECK(finite(a) && finite(b));
        const double plain2 = goertzel(a.l, 880.0, 4800) / goertzel(a.l, 440.0, 4800);
        const double oct2 = goertzel(b.l, 880.0, 4800) / goertzel(b.l, 440.0, 4800);
        CHECK(plain2 < 0.05);  // symmetric clipping: odd harmonics only
        CHECK(oct2 > 0.5);     // the octave is there
        const double ratio = rms(a.l, 4800) / rms(sine(440.0, 0.5, 0.3).l, 4800);
        CHECK(ratio > 0.5 && ratio < 2.0);
    }
    CHECK(w.empty());

    // --- Parameter checks.
    w.clear();
    CHECK(make("echo", R"({"time_ms": 5000, "feedbak": 0.3})", w) != nullptr);
    CHECK(hasWarning(w, "\"time_ms\" 5000 is outside 20..2000 - 2000 used"));
    CHECK(hasWarning(w, "unknown parameter \"feedbak\""));
    w.clear();
    CHECK(make("chorus", R"({"mode": "dimension"})", w) != nullptr);
    CHECK(hasWarning(w, "\"mode\" must be one of"));
    w.clear();
    CHECK(createEffect("flanger", json::object(), "p", w) == nullptr);
    CHECK(hasWarning(w, "unknown effect type \"flanger\""));
    CHECK(w.size() == 1);

    // --- Chain from definitions: bypass leaves an effect out.
    {
        w.clear();
        std::vector<FxDef> defs(3);
        defs[0].type = "tone";
        defs[1].type = "echo";
        defs[1].bypass = true;
        defs[2].type = "chorus";
        defs[2].paramsJson = R"({"mode":"ensemble"})";
        auto chain = FxChain::build(defs, "p", w);
        CHECK(chain->size() == 2);
        CHECK(w.empty());
    }

    // --- Switch: parsing.
    {
        w.clear();
        auto def = [](const char* type, const char* params, const char* param, const char* a, const char* b) {
            FxDef d;
            d.type = type;
            d.paramsJson = params;
            d.switchParam = param;
            d.switchA = a;
            d.switchB = b;
            return d;
        };
        auto chain = FxChain::build({def("leslie", R"({"speed":"slow"})", "speed", "\"slow\"", "\"fast\""),
                                     def("echo", "{}", "mix", "0.1", "0.4"),
                                     def("tremolo", "{}", "bypass", "true", "false")},
                                    "p", w);
        CHECK(chain->size() == 3 && chain->hasSwitch());
        CHECK(w.empty());
        FxChain::build({def("echo", R"({"mix":0.3})", "mix", "0.1", "3")}, "p", w);
        CHECK(hasWarning(w, "\"mix\" is 0.3 in params but the switch starts from 0.1"));
        CHECK(hasWarning(w, "(switch value B)"));
        w.clear();
        FxChain::build({def("echo", "{}", "bypass", "1", "0")}, "p", w);
        CHECK(hasWarning(w, "needs true and false"));
        w.clear();
    }

    // --- Switch: bypass goes from effect to dry with a short crossfade.
    {
        std::vector<FxDef> defs(1);
        defs[0].type = "tremolo";
        defs[0].paramsJson = R"({"rate_hz": 4, "depth": 1})";
        defs[0].switchParam = "bypass";
        defs[0].switchA = "false";
        defs[0].switchB = "true";
        auto chain = FxChain::build(defs, "p", w);
        chain->prepare(kRate);
        Stereo s(24000);
        std::fill(s.l.begin(), s.l.end(), 0.5f);
        std::fill(s.r.begin(), s.r.end(), 0.5f);
        chain->process(s.l.data(), s.r.data(), 12000);  // A: tremolo
        CHECK(*std::min_element(s.l.begin(), s.l.begin() + 12000) < 0.05f);
        chain->setSwitch(true, false);
        for (int pos = 12000; pos < 24000; pos += 256) chain->process(s.l.data() + pos, s.r.data() + pos, 256);
        float jump = 0;
        for (int i = 12001; i < 24000; ++i) jump = std::max(jump, std::fabs(s.l[i] - s.l[i - 1]));
        CHECK(jump < 0.02f);                                  // no click
        CHECK(std::fabs(s.l[23999] - 0.5f) < 1e-6f);          // B: dry
    }

    // --- Switch: Leslie speed changes gradually and reaches the other speed.
    {
        std::vector<FxDef> defs(1);
        defs[0].type = "leslie";
        defs[0].switchParam = "speed";
        defs[0].switchA = "\"slow\"";
        defs[0].switchB = "\"fast\"";
        defs[0].paramsJson = R"({"cabinet": 0})";  // direct sound only: one amplitude peak per turn
        auto chain = FxChain::build(defs, "p", w);
        chain->prepare(kRate);
        auto peaksPerSecond = [&](double seconds) {
            Stereo s = sine(3000.0, seconds, 0.3);
            for (int pos = 0; pos < s.size(); pos += 256) chain->process(s.l.data() + pos, s.r.data() + pos, std::min(256, s.size() - pos));
            const int win = 480;  // 10 ms
            std::vector<double> env;
            for (int b = 0; (b + 1) * win <= s.size(); ++b) env.push_back(rms(s.l, b * win, (b + 1) * win));
            int peaks = 0;
            for (std::size_t i = 2; i + 2 < env.size(); ++i)
                if (env[i] > env[i - 1] && env[i] >= env[i + 1] && env[i] > env[i - 2] && env[i] >= env[i + 2]) ++peaks;
            return peaks / seconds;
        };
        const double slow = peaksPerSecond(4.0);
        chain->setSwitch(true, false);
        const double during = peaksPerSecond(0.5);
        const double fast = peaksPerSecond(4.0);
        CHECK(slow < 2.0);
        CHECK(fast > 5.0 && fast < 9.0);
        CHECK(during < fast);  // still speeding up
        std::printf("leslie switch: %.1f -> %.1f -> %.1f modulation peaks per second\n", slow, during, fast);
    }

    // --- Engine: the switch toggles, and a program change brings it back to A.
    {
        auto inst = sineInstrument(440.0);
        Layer layer;
        layer.instrument = inst.get();
        auto make = [&](const char* id, bool withSwitch) {
            auto p = std::make_unique<Program>();
            p->id = p->name = id;
            p->layers.push_back(layer);
            FxDef d;
            d.type = "echo";
            if (withSwitch) {
                d.switchParam = "mix";
                d.switchA = "0";
                d.switchB = "0.5";
            }
            p->fx.push_back(d);
            p->fxChain = FxChain::build(p->fx, id, w);
            return p;
        };
        auto one = make("one", true), two = make("two", false);
        PresetTable presets{};
        presets[0] = one.get();
        presets[1] = two.get();
        Engine engine;
        engine.prepare(kRate);
        engine.setPresets(&presets);
        CHECK(engine.selectPresetNow(1));
        const int src = engine.registerSource();
        CHECK(!engine.fxSwitchB());
        engine.post(src, EngineEvent::fxSwitch());
        render(engine, 0.05);
        CHECK(engine.fxSwitchB());
        engine.post(src, EngineEvent::selectPreset(2));
        render(engine, 0.05);
        CHECK(!engine.fxSwitchB());
        engine.post(src, EngineEvent::fxSwitch());  // program without a switch: ignored
        render(engine, 0.05);
        CHECK(!engine.fxSwitchB());
    }

    // --- Engine: a program change fades the old echo out; without a change the tail rings.
    {
        auto inst = sineInstrument(440.0);
        Layer layer;
        layer.instrument = inst.get();
        layer.adsr.releaseMs = 5.0f;
        layer.loop = true;

        auto program = [&](const std::string& id, const char* fxType, const char* params) {
            auto p = std::make_unique<Program>();
            p->id = p->name = id;
            p->layers.push_back(layer);
            if (fxType) {
                FxDef d;
                d.type = fxType;
                d.paramsJson = params;
                p->fx.push_back(d);
            }
            p->fxChain = FxChain::build(p->fx, id, w);
            return p;
        };
        auto echo = program("echo", "echo", R"({"time_ms": 60, "feedback": 0.8, "mix": 1, "wow": 0})");
        auto plain = program("plain", nullptr, "");
        PresetTable presets{};
        presets[0] = echo.get();
        presets[1] = plain.get();

        Engine engine;
        engine.setPresets(&presets);  // before prepare: chains are prepared there
        engine.prepare(kRate);
        engine.setMasterGainDb(0.0f);
        CHECK(engine.selectPresetNow(1));
        const int src = engine.registerSource();

        // Tail without a program change.
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        render(engine, 0.2);
        engine.post(src, EngineEvent::fromMidi({0x80, 60, 0}));
        auto tail = render(engine, 0.3);
        CHECK(peak(tail, 4800) > 0.05f);  // echoes still sounding after the note

        // All sound off: the echo tail is silenced too (then it rings again normally).
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        render(engine, 0.2);
        engine.post(src, EngineEvent::allSoundOff());
        auto silenced = render(engine, 0.2);
        CHECK(peak(silenced, static_cast<int>(0.04 * kRate)) < 1e-6f);
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        render(engine, 0.2);
        engine.post(src, EngineEvent::fromMidi({0x80, 60, 0}));
        auto again = render(engine, 0.3);
        CHECK(peak(again, 4800) > 0.05f);

        // Program change: the tail is gone after the fade.
        engine.post(src, EngineEvent::selectPreset(2));
        auto after = render(engine, 0.2);
        CHECK(peak(after, static_cast<int>(0.04 * kRate)) < 1e-6f);
        CHECK(engine.activeVoices() == 0);

        // Back to the echo program: its chain starts clean (no old tail).
        engine.post(src, EngineEvent::selectPreset(1));
        auto back = render(engine, 0.2);
        CHECK(peak(back) < 1e-6f);

        // The new program's notes play through the new chain at once.
        engine.post(src, EngineEvent::selectPreset(2));
        engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
        auto plainNote = render(engine, 0.1);
        CHECK(peak(plainNote, 2400) > 0.3f);
    }

    // --- Cost of the whole set, for information.
    {
        std::vector<FxDef> defs;
        for (const char* t : {"tone", "echo", "chorus", "phaser", "leslie", "tremolo", "fuzz", "width", "room"}) {
            FxDef d;
            d.type = t;
            defs.push_back(d);
        }
        FxDef ens;
        ens.type = "chorus";
        ens.paramsJson = R"({"mode":"ensemble"})";
        defs.push_back(ens);
        auto chain = FxChain::build(defs, "bench", w);
        chain->prepare(kRate);
        Stereo s = noise(10.0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int pos = 0; pos < s.size(); pos += 128) chain->process(s.l.data() + pos, s.r.data() + pos, 128);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("all 10 effects in a chain: %.1f%% of real time on this machine\n", 100.0 * secs / 10.0);
    }

    return finish("test_fx");
}
