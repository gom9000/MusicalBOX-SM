#include "fx/Effects.h"

#include <cmath>

namespace mbx {

using dsp::Biquad;

namespace {
// Rotation rates of a Leslie 122 at the two speeds.
constexpr float kHornSlowHz = 0.83f, kHornFastHz = 6.75f;
constexpr float kDrumSlowHz = 0.67f, kDrumFastHz = 5.6f;
constexpr float kCrossoverHz = 800.0f;

// What each microphone hears besides the direct sound: extra delay (ms),
// angle at which the reflection sees the rotor (turns), level. Side walls,
// back wall (strongest when the horn points away from the microphone),
// floor and top, then the room. The second microphone sees the cabinet
// mirrored, with slightly different distances.
struct ReflectionDef {
    float ms, angle, gain;
};
constexpr ReflectionDef kReflectionDefs[] = {
    {1.4f, 0.25f, 0.5f}, {1.9f, -0.25f, 0.45f}, {2.6f, 0.5f, 0.6f}, {4.3f, 0.12f, 0.35f},
    {7.9f, 0.4f, 0.3f},  {12.7f, 0.7f, 0.25f},  {17.3f, 0.9f, 0.2f},
};
constexpr float kReflectionToneHz = 2500.0f;

float onePoleCoef(double sampleRate, double hz) {
    return static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979323846 * hz / sampleRate));
}
} // namespace

Leslie::Leslie(FxParams& p)
    : fast_(p.choice("speed", "slow", {"slow", "fast"}) == "fast"),
      depth_(p.number("depth", 1.0f, 0.0f, 2.0f)),
      spread_(p.number("spread", 0.8f, 0.0f, 1.0f)),
      drive_(p.number("drive", 0.0f, 0.0f, 1.0f)),
      mix_(p.number("mix", 1.0f, 0.0f, 1.0f)),
      cabinet_(p.number("cabinet", 1.0f, 0.0f, 2.0f)),
      topHz_(p.number("top_hz", 2800.0f, 1000.0f, 20000.0f)) {}

void Leslie::prepare(double sr) {
    sampleRate_ = sr;
    lowA_.set(Biquad::Type::LowPass, sr, kCrossoverHz, 0.7071);
    lowB_.set(Biquad::Type::LowPass, sr, kCrossoverHz, 0.7071);
    highA_.set(Biquad::Type::HighPass, sr, kCrossoverHz, 0.7071);
    highB_.set(Biquad::Type::HighPass, sr, kCrossoverHz, 0.7071);

    // Depth 1 is a real cabinet: horn mouth about 19 cm from the axis
    // (Doppler about +-0.55 ms), strong horn amplitude change, milder drum.
    const float ms = static_cast<float>(0.001 * sr);
    hornBase_ = 1.2f * ms;
    drumBase_ = 1.2f * ms;
    // With the cabinet reflections the horn swings less between the
    // microphones (the reflections fill in) and the drum is heard more.
    const float cab = std::min(1.0f, cabinet_);
    hornDoppler_ = depth_ * 0.55f * ms;
    drumDoppler_ = depth_ * (0.2f + 0.2f * cab) * ms;
    hornAm_ = std::min(0.95f, (0.7f - 0.3f * cab) * depth_);
    drumAm_ = std::min(0.9f, (0.45f + 0.35f * cab) * depth_);
    hornMakeup_ = 1.0f / (1.0f - 0.5f * hornAm_);
    drumMakeup_ = 1.0f / (1.0f - 0.5f * drumAm_);
    // Facing away, the horn loses its top: low-pass from 2.5 kHz (away) to 14 kHz (facing).
    darkCoef_ = onePoleCoef(sr, 14000.0 - std::min(1.0f, depth_) * 11500.0);
    brightCoef_ = onePoleCoef(sr, 14000.0);

    const float micOffset = 0.15f + 0.35f * spread_;
    float longest = 0.0f;
    for (int k = 0; k < kReflections; ++k) {
        const ReflectionDef& d = kReflectionDefs[k];
        Reflection& r = refl_[static_cast<std::size_t>(k)];
        r.gain = d.gain * cabinet_;
        for (int c = 0; c < 2; ++c) {
            const float angle = c == 0 ? d.angle : micOffset - d.angle;
            r.cosA[c] = std::cos(dsp::kTwoPi * angle);
            r.sinA[c] = std::sin(dsp::kTwoPi * angle);
            const float jitter = c == 0 ? 0.0f : 0.5f * std::fmod(k * 0.37f, 0.3f);
            r.delay[c] = d.ms * (1.0f + jitter) * ms;
            longest = std::max(longest, r.delay[c]);
        }
    }
    reflCoef_ = onePoleCoef(sr, kReflectionToneHz);
    // The reflections add energy: keep the overall level about the same.
    float energy = 0.0f;
    for (const auto& r : refl_) energy += r.gain * r.gain;
    cabinetMakeup_ = 1.0f / std::sqrt(1.0f + 0.5f * energy);
    topOn_ = topHz_ < 18000.0f;
    for (auto& t : top_) t.set(Biquad::Type::LowPass, sr, topHz_, 0.7071);

    horn_.resize(static_cast<std::size_t>(hornBase_ + hornDoppler_ + longest) + 8);
    drum_.resize(static_cast<std::size_t>(drumBase_ + drumDoppler_ + longest) + 8);
    hornLfo_.set(sr, fast_ ? kHornFastHz : kHornSlowHz);
    drumLfo_.set(sr, fast_ ? kDrumFastHz : kDrumSlowHz, 0.4);
    // Speed changes (switch): the light horn gets there in about a second, the
    // heavy drum takes several. Time constants of an exponential approach.
    auto coef = [sr](double seconds) { return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sr))); };
    hornUp_ = coef(0.35);
    hornDown_ = coef(0.6);
    drumUp_ = coef(1.5);
    drumDown_ = coef(2.0);
    reset();
}

bool Leslie::configureSwitch(const std::string& param, const nlohmann::json& a, const nlohmann::json& b) {
    auto speed = [](const nlohmann::json& v, bool& fast) {
        if (!v.is_string() || (v != "slow" && v != "fast")) return false;
        fast = v == "fast";
        return true;
    };
    if (param != "speed" || !speed(a, fastA_) || !speed(b, fastB_)) return false;
    switchable_ = true;
    return true;
}

void Leslie::setSwitch(bool positionB, bool immediate) {
    if (switchable_) setRates(positionB ? fastB_ : fastA_, immediate);
}

void Leslie::setRates(bool fast, bool immediate) {
    hornTarget_ = fast ? kHornFastHz : kHornSlowHz;
    drumTarget_ = fast ? kDrumFastHz : kDrumSlowHz;
    if (immediate) {
        hornHz_ = hornTarget_;
        drumHz_ = drumTarget_;
        hornLfo_.setHz(sampleRate_, hornHz_);
        drumLfo_.setHz(sampleRate_, drumHz_);
    }
}

void Leslie::reset() {
    setRates(switchable_ ? fastA_ : fast_, true);
    lowA_.reset(); lowB_.reset(); highA_.reset(); highB_.reset();
    horn_.reset();
    drum_.reset();
    hornLfo_.reset();
    drumLfo_.reset();
    hornTone_.fill(0.0f);
    hornReflTone_.fill(0.0f);
    drumReflTone_.fill(0.0f);
    for (auto& t : top_) t.reset();
}

// Sum of the reflections of one rotor at one microphone. s, c: sine and
// cosine of the rotor angle; direction -1 for the drum, which turns the
// other way.
float Leslie::reflections(const dsp::DelayLine& line, float base, float doppler, float am, float s, float c,
                          float direction, int mic) const {
    float sum = 0.0f;
    for (const Reflection& r : refl_) {
        const float sa = s * r.cosA[mic] + c * r.sinA[mic];           // sin(rotor + angle)
        const float facing = 0.5f + 0.5f * (c * r.cosA[mic] - s * r.sinA[mic]);  // cos(rotor + angle)
        sum += r.gain * line.read(base + r.delay[mic] + doppler * direction * sa) * (1.0f - am * (1.0f - facing));
    }
    return sum;
}

void Leslie::process(float* left, float* right, int frames) {
    const float k = 1.0f + 6.0f * drive_;
    // Second microphone: from 0.15 turn (close, narrow) to 0.5 turn (opposite sides, wide).
    const float micOffset = 0.15f + 0.35f * spread_;

    for (int i = 0; i < frames; ++i) {
        if (hornHz_ != hornTarget_ || drumHz_ != drumTarget_) {
            hornHz_ += (hornTarget_ - hornHz_) * (hornTarget_ > hornHz_ ? hornUp_ : hornDown_);
            drumHz_ += (drumTarget_ - drumHz_) * (drumTarget_ > drumHz_ ? drumUp_ : drumDown_);
            bool arrived = false;
            if (std::fabs(hornTarget_ - hornHz_) < 1e-3f) hornHz_ = hornTarget_, arrived = true;
            if (std::fabs(drumTarget_ - drumHz_) < 1e-3f) drumHz_ = drumTarget_, arrived = true;
            if ((i & 31) == 0 || arrived) {
                hornLfo_.setHz(sampleRate_, hornHz_);
                drumLfo_.setHz(sampleRate_, drumHz_);
            }
        }
        float m = 0.5f * (left[i] + right[i]);
        if (drive_ > 0.0f) m = dsp::softClip(k * m) / k;  // the amplifier: only loud passages distort

        horn_.push(highB_.process(highA_.process(m)));
        drum_.push(lowB_.process(lowA_.process(m)));

        const bool cabinet = cabinet_ > 0.0f;
        float hs0 = 0.0f, hc0 = 0.0f, ds0 = 0.0f, dc0 = 0.0f;
        if (cabinet) {
            hs0 = std::sin(dsp::kTwoPi * hornLfo_.phase());
            hc0 = std::cos(dsp::kTwoPi * hornLfo_.phase());
            ds0 = std::sin(dsp::kTwoPi * drumLfo_.phase());
            dc0 = std::cos(dsp::kTwoPi * drumLfo_.phase());
        }
        float out[2];
        for (int c = 0; c < 2; ++c) {
            const float off = c == 0 ? 0.0f : micOffset;
            // Horn: facing = 1 when the mouth points at this microphone.
            const float hs = hornLfo_.sine(off);
            const float facing = 0.5f + 0.5f * hornLfo_.sine(off + 0.25f);
            float h = horn_.read(hornBase_ + hornDoppler_ * hs);
            const float a = darkCoef_ + (brightCoef_ - darkCoef_) * facing;
            hornTone_[c] += a * (h - hornTone_[c]);
            h = hornTone_[c] * (1.0f - hornAm_ * (1.0f - facing)) * hornMakeup_;
            // Drum: turns the other way, mostly amplitude.
            const float ds = -drumLfo_.sine(off);
            const float dFacing = 0.5f + 0.5f * drumLfo_.sine(off + 0.25f);
            const float d = drum_.read(drumBase_ + drumDoppler_ * ds) * (1.0f - drumAm_ * (1.0f - dFacing)) * drumMakeup_;
            if (cabinet) {
                const float hr = reflections(horn_, hornBase_, hornDoppler_, hornAm_, hs0, hc0, 1.0f, c);
                const float dr = reflections(drum_, drumBase_, drumDoppler_, drumAm_, ds0, dc0, -1.0f, c);
                hornReflTone_[c] += reflCoef_ * (hr * hornMakeup_ - hornReflTone_[c]);
                drumReflTone_[c] += reflCoef_ * (dr * drumMakeup_ - drumReflTone_[c]);
                h = (h + hornReflTone_[c]) * cabinetMakeup_;
                out[c] = (d + drumReflTone_[c]) * cabinetMakeup_;
            } else {
                out[c] = d;
            }
            if (topOn_) h = top_[c].process(h);
            out[c] += h;
        }
        hornLfo_.advance();
        drumLfo_.advance();

        left[i] = (1.0f - mix_) * left[i] + mix_ * out[0];
        right[i] = (1.0f - mix_) * right[i] + mix_ * out[1];
    }
}

} // namespace mbx
