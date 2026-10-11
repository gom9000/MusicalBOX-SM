// Master limiter: output never above the threshold, untouched below it,
// no sudden gain jumps, gain recovers after a loud passage.
#include "core/Limiter.h"

#include <cmath>
#include <cstdio>
#include <vector>

using mbx::Limiter;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static std::vector<float> sine(int frames, float amp, double freq, double rate) {
    std::vector<float> v(2 * frames);
    for (int i = 0; i < frames; ++i) {
        const float s = amp * static_cast<float>(std::sin(2.0 * M_PI * freq * i / rate));
        v[2 * i] = s;
        v[2 * i + 1] = s;
    }
    return v;
}

static float peakOf(const std::vector<float>& v, int from = 0) {
    float p = 0.0f;
    for (std::size_t i = 2 * from; i < v.size(); ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}

int main() {
    const double rate = 48000.0;
    const float threshold = std::pow(10.0f, -1.0f / 20.0f);

    // Quiet signal: unchanged, only delayed.
    {
        Limiter lim;
        lim.prepare(rate);
        auto in = sine(4800, 0.5f, 440.0, rate);
        auto out = in;
        for (int pos = 0; pos < 4800; pos += 128) lim.process(out.data() + 2 * pos, std::min(128, 4800 - pos));
        const int d = Limiter::kLookahead - 1;
        float err = 0.0f;
        for (int i = d; i < 4800; ++i) err = std::max(err, std::fabs(out[2 * i] - in[2 * (i - d)]));
        CHECK(err < 1e-6f);
    }

    // Loud signal (+10 dB over full scale, sudden start): never above the threshold.
    {
        Limiter lim;
        lim.prepare(rate);
        auto out = sine(1000, 0.0f, 440.0, rate);
        auto loud = sine(9600, 3.0f, 440.0, rate);
        out.insert(out.end(), loud.begin(), loud.end());
        float lowest = 1.0f;
        const int frames = static_cast<int>(out.size() / 2);
        for (int pos = 0; pos < frames; pos += 256) lowest = std::min(lowest, lim.process(out.data() + 2 * pos, std::min(256, frames - pos)));
        CHECK(peakOf(out) <= threshold + 1e-4f);
        CHECK(lowest < 0.35f);                       // about -10 dB of reduction
        CHECK(peakOf(out, frames - 2400) > 0.85f);   // and still loud, not squashed to nothing
    }

    // Gain moves smoothly: sample-to-sample change of a steady DC level stays tiny.
    {
        Limiter lim;
        lim.prepare(rate);
        std::vector<float> out(2 * 4800, 0.2f);
        for (int i = 2000; i < 4800; ++i) out[2 * i] = out[2 * i + 1] = 2.0f;
        lim.process(out.data(), 4800);
        float jump = 0.0f;
        for (int i = 2100; i < 4800; ++i) jump = std::max(jump, std::fabs(out[2 * i] - out[2 * i - 2]));
        CHECK(jump < 0.05f);
    }

    // After a loud burst the gain comes back to 1.
    {
        Limiter lim;
        lim.prepare(rate);
        auto out = sine(4800, 3.0f, 440.0, rate);
        lim.process(out.data(), 4800);
        auto quiet = sine(48000, 0.5f, 440.0, rate);
        float lowest = 1.0f;
        for (int pos = 0; pos + 512 <= 48000; pos += 512) {
            lowest = lim.process(quiet.data() + 2 * pos, 512);
        }
        CHECK(lowest > 0.999f);
    }

    if (failures == 0) std::printf("test_limiter: all passed\n");
    return failures == 0 ? 0 : 1;
}
