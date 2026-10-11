// Engine behaviour on synthetic instruments: pitch, sample-rate compensation,
// envelope, sustain pedal, one-shot vs loop, velocity, polyphony, volume.

#include "check.h"
#include "core/Engine.h"
#include "sampler/Instrument.h"
#include "sampler/Program.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace mbx;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

Zone makeSine(double freq, double fileRate, double seconds, int root, float amplitude = 0.5f) {
    Zone z;
    z.channels = 1;
    z.sampleRate = fileRate;
    z.frames = static_cast<std::uint32_t>(seconds * fileRate);
    z.rootNote = root;
    z.data.resize(z.frames);
    for (std::uint32_t i = 0; i < z.frames; ++i)
        z.data[i] = static_cast<std::int16_t>(std::lround(amplitude * 32767.0 * std::sin(2 * kPi * freq * i / fileRate)));
    return z;
}

struct Rig {
    std::unique_ptr<Engine> engine = std::make_unique<Engine>();
    Program program;
    int source = -1;

    explicit Rig(const Instrument* instrument, bool loop = false) {
        Layer layer;
        layer.instrument = instrument;
        layer.loop = loop;
        program.layers.push_back(layer);
        engine->prepare(kRate);
        engine->setProgram(&program);
        engine->setMasterGainDb(0.0f);
        source = engine->registerSource();
    }

    void send(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
        engine->post(source, EngineEvent::fromMidi({status, d1, d2}));
    }

    // Renders `seconds` of audio; returns the left channel.
    std::vector<float> run(double seconds) {
        const int frames = static_cast<int>(seconds * kRate);
        std::vector<float> stereo(static_cast<std::size_t>(frames) * 2);
        for (int pos = 0; pos < frames; pos += 128) {
            const int n = std::min(128, frames - pos);
            engine->render(stereo.data() + 2 * pos, n);
        }
        std::vector<float> left(frames);
        for (int i = 0; i < frames; ++i) left[i] = stereo[2 * i];
        return left;
    }
};

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
    if (crossings < 2) return 0.0;
    return (crossings - 1) * kRate / static_cast<double>(last - first);
}

float peak(const std::vector<float>& x, std::size_t from, std::size_t to) {
    float p = 0.0f;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

} // namespace

int main() {
    // One 250 Hz zone at C3: every other note is filled from it.
    Instrument sine("SINE", "sine");
    sine.addZone(makeSine(250.0, kRate, 1.0, 60));
    sine.finalize(FillMode::Nearest);

    // --- Pitch and transposition ---------------------------------------------
    {
        Rig rig(&sine);
        rig.send(0x90, 60, 127);
        auto x = rig.run(0.3);
        CHECK_NEAR(frequency(x, 4800, 14400), 250.0, 1.0);
    }
    {
        Rig rig(&sine);
        rig.send(0x90, 72, 127);
        auto x = rig.run(0.3);
        CHECK_NEAR(frequency(x, 4800, 14400), 500.0, 2.0);
    }
    {
        Rig rig(&sine);
        rig.send(0x90, 67, 127);
        auto x = rig.run(0.3);
        CHECK_NEAR(frequency(x, 4800, 14400), 250.0 * std::pow(2.0, 7.0 / 12.0), 1.5);
    }

    // --- A 44.1 kHz file plays at the right pitch on a 48 kHz engine -----------
    {
        Instrument cd("CD", "44.1k");
        cd.addZone(makeSine(250.0, 44100.0, 1.0, 60));
        cd.finalize(FillMode::Nearest);
        Rig rig(&cd);
        rig.send(0x90, 60, 127);
        auto x = rig.run(0.3);
        CHECK_NEAR(frequency(x, 4800, 14400), 250.0, 1.0);
    }

    // --- FillMode::None leaves other notes silent -----------------------------
    {
        Instrument strict("STRICT", "strict");
        strict.addZone(makeSine(250.0, kRate, 1.0, 60));
        strict.finalize(FillMode::None);
        CHECK(strict.findZone(60, 100) != nullptr);
        CHECK(strict.findZone(61, 100) == nullptr);
    }

    // --- Release: silent shortly after note-off (default release 150 ms) -----
    {
        Rig rig(&sine);
        rig.send(0x90, 60, 127);
        auto on = rig.run(0.2);
        CHECK(peak(on, 4800, on.size()) > 0.4f);
        rig.send(0x80, 60, 0);
        auto off = rig.run(0.3);
        CHECK(peak(off, static_cast<std::size_t>(0.2 * kRate), off.size()) < 1e-3f);
        CHECK(rig.engine->activeVoices() == 0);
    }

    // --- Sustain pedal holds released notes until it is lifted ----------------
    {
        Rig rig(&sine);
        rig.send(0xB0, 64, 127);
        rig.send(0x90, 60, 127);
        rig.run(0.1);
        rig.send(0x80, 60, 0);
        auto held = rig.run(0.3);
        CHECK(peak(held, static_cast<std::size_t>(0.2 * kRate), held.size()) > 0.4f);
        rig.send(0xB0, 64, 0);
        auto lifted = rig.run(0.3);
        CHECK(peak(lifted, static_cast<std::size_t>(0.2 * kRate), lifted.size()) < 1e-3f);
    }

    // --- One-shot sample stops at its end, even with the key held ------------
    Instrument shortSine("SHORT", "short");
    {
        Zone z = makeSine(250.0, kRate, 0.5, 60);
        z.hasLoop = true;            // 50 periods of 192 samples
        z.loopStart = 4800;
        z.loopEnd = 4800 + 192 * 50 - 1;
        shortSine.addZone(std::move(z));
        shortSine.finalize(FillMode::Nearest);
    }
    {
        Rig rig(&shortSine, /*loop=*/false);
        rig.send(0x90, 60, 127);
        auto x = rig.run(1.0);
        CHECK(peak(x, static_cast<std::size_t>(0.55 * kRate), x.size()) < 1e-6f);
        CHECK(rig.engine->activeVoices() == 0);
    }

    // --- Organ mode: the same sample is held by its loop ----------------------
    {
        Rig rig(&shortSine, /*loop=*/true);
        rig.send(0x90, 60, 127);
        auto x = rig.run(2.0);
        CHECK(peak(x, static_cast<std::size_t>(1.5 * kRate), x.size()) > 0.4f);
        CHECK_NEAR(frequency(x, static_cast<std::size_t>(1.0 * kRate), static_cast<std::size_t>(1.9 * kRate)), 250.0, 1.0);
        CHECK(rig.engine->activeVoices() == 1);
    }

    // --- Velocity mapping -----------------------------------------------------
    {
        VelocitySettings off;
        CHECK(off.map(10).lookupVelocity == 127);
        CHECK_NEAR(off.map(10).gain, 1.0, 1e-6);

        VelocitySettings lin{true, 4, VelocityCurve::Linear};
        CHECK(lin.map(127).lookupVelocity == 127);
        CHECK_NEAR(lin.map(127).gain, 1.0, 1e-6);
        CHECK_NEAR(lin.map(1).gain, 0.25, 1e-6);
        CHECK(lin.map(1).lookupVelocity == 31);
        CHECK_NEAR(lin.map(64).gain, 0.75, 1e-6);  // ceil(64*4/127) = 3

        // Two steps must stay inside two sample layers split at 64.
        VelocitySettings two{true, 2, VelocityCurve::Linear};
        CHECK(two.map(40).lookupVelocity == 63);
        CHECK(two.map(63).lookupVelocity == 63);
        CHECK(two.map(64).lookupVelocity == 127);

        VelocitySettings ex{true, 4, VelocityCurve::Exp};
        CHECK_NEAR(ex.map(1).gain, std::pow(10.0, -1.5), 1e-4);  // -30 dB
        CHECK_NEAR(ex.map(127).gain, 1.0, 1e-6);
    }

    // --- Velocity layers choose the zone --------------------------------------
    {
        Instrument piano("PIANO", "two layers");
        Zone soft = makeSine(250.0, kRate, 1.0, 60, 0.1f);
        soft.loVel = 1;
        soft.hiVel = 63;
        Zone loud = makeSine(250.0, kRate, 1.0, 60, 0.8f);
        loud.loVel = 64;
        loud.hiVel = 127;
        piano.addZone(std::move(soft));
        piano.addZone(std::move(loud));
        piano.finalize(FillMode::Nearest);
        CHECK(piano.findZone(62, 30)->hiVel == 63);
        CHECK(piano.findZone(62, 100)->loVel == 64);

        Rig rig(&piano);
        rig.program.layers[0].velocity = {true, 127, VelocityCurve::Linear};
        rig.send(0x90, 60, 127);
        auto x = rig.run(0.2);
        CHECK_NEAR(peak(x, 4800, x.size()), 0.8, 0.02);
    }

    // --- Polyphony is capped and stolen voices fade out ----------------------
    {
        Rig rig(&shortSine, /*loop=*/true);
        for (int i = 0; i < 100; ++i) rig.send(0x90, static_cast<std::uint8_t>(20 + i), 100);
        rig.run(0.01);
        CHECK(rig.engine->activeVoices() <= kDefaultPolyphony + kStealReserve);
        rig.run(0.05);
        CHECK(rig.engine->activeVoices() == kDefaultPolyphony);
    }

    // --- Polyphony is a setting ------------------------------------------------
    {
        Rig rig(&shortSine, /*loop=*/true);
        rig.engine->setPolyphony(16);
        CHECK(rig.engine->polyphony() == 16);
        for (int i = 0; i < 30; ++i) rig.send(0x90, static_cast<std::uint8_t>(40 + i), 100);
        rig.run(0.05);
        CHECK(rig.engine->activeVoices() == 16);
        rig.engine->setPolyphony(1);  // clamped to the minimum
        CHECK(rig.engine->polyphony() == kMinPolyphony);
    }

    // --- MIDI channel filter ------------------------------------------------------
    {
        Rig rig(&sine);
        rig.engine->setMidiChannel(2);
        rig.send(0x90, 60, 127);  // channel 1: ignored
        rig.run(0.05);
        CHECK(rig.engine->activeVoices() == 0);
        rig.send(0x91, 60, 127);  // channel 2
        rig.run(0.05);
        CHECK(rig.engine->activeVoices() == 1);
        rig.engine->setMidiChannel(0);  // omni
        rig.send(0x9F, 64, 127);  // channel 16
        rig.run(0.05);
        CHECK(rig.engine->activeVoices() == 2);
    }

    // --- Master volume --------------------------------------------------------
    {
        Rig rig(&sine);
        rig.engine->setMasterGainDb(-6.0f);
        rig.send(0x90, 60, 127);
        auto x = rig.run(0.2);
        CHECK_NEAR(peak(x, 4800, x.size()), 0.5 * std::pow(10.0, -6.0 / 20.0), 0.01);

        rig.engine->post(rig.source, EngineEvent::masterGainDb(0.0f));
        auto y = rig.run(0.2);
        CHECK_NEAR(peak(y, 4800, y.size()), 0.5, 0.01);
    }

    // --- All sound off ---------------------------------------------------------
    {
        Rig rig(&shortSine, /*loop=*/true);
        rig.send(0x90, 60, 127);
        rig.send(0x90, 64, 127);
        rig.run(0.1);
        rig.send(0xB0, 120, 0);
        auto x = rig.run(0.1);
        CHECK(peak(x, static_cast<std::size_t>(0.02 * kRate), x.size()) < 1e-3f);
        CHECK(rig.engine->activeVoices() == 0);
    }

    return finish("test_engine");
}
