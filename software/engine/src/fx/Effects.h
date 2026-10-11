#pragma once

// The effects of MusicalBOX SM. Each one reads its parameters from the
// "params" object in programs.json (see README, "Effetti", for the list).

#include "fx/Dsp.h"
#include "fx/Effect.h"

#include <array>

namespace mbx {

// Equaliser: low cut, bass shelf, mid peak, treble shelf, high cut, output gain.
class Tone : public Effect {
public:
    explicit Tone(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    float lowCutHz_, bassDb_, bassHz_, midDb_, midHz_, trebleDb_, trebleHz_, highCutHz_, gain_;
    struct Channel {
        dsp::Biquad lowCut, bass, mid, treble, highCut;
    };
    std::array<Channel, 2> ch_;
};

// Tape echo: one playback head, feedback through a darkening, saturating loop,
// slight wow and flutter. Optional ping-pong between left and right.
class TapeEcho : public Effect {
public:
    explicit TapeEcho(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    float timeMs_, feedback_, mix_, toneHz_, lowCutHz_, wow_, drive_;
    bool pingPong_;
    float delaySamples_ = 0.0f, wowDepth_ = 0.0f, flutterDepth_ = 0.0f;
    std::array<dsp::DelayLine, 2> line_;
    std::array<dsp::Biquad, 2> dark_, thin_;
    dsp::Lfo wowLfo_, flutterLfo_;
};

// Chorus. "ce2": one modulated delay, the classic chorus pedal (stereo by
// opposite LFO phases). "ensemble": three delays at 120 degrees with a slow
// and a fast modulation, the string-ensemble chorus.
class Chorus : public Effect {
public:
    explicit Chorus(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    bool ensemble_;
    float rateHz_, depth_, mix_;
    float baseDelay_ = 0.0f, slowDepth_ = 0.0f, fastDepth_ = 0.0f, makeup_ = 1.0f;
    std::array<dsp::DelayLine, 2> line_;
    std::array<dsp::OnePole, 2> dark_;
    dsp::Lfo slow_, fast_;
};

// Phaser, the classic four-stage pedal: four all-pass stages swept by an
// LFO, mixed 50/50 with the dry signal, with optional feedback.
class Phaser : public Effect {
public:
    explicit Phaser(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    static constexpr int kMaxStages = 12;
    float rateHz_, depth_, feedback_, mix_;
    int stages_;
    bool stereo_;
    double sampleRate_ = 48000.0;
    float makeup_ = 1.0f;
    struct Channel {
        std::array<float, kMaxStages> x1{}, y1{};
        float coef = 0.0f;
        float last = 0.0f;
    };
    std::array<Channel, 2> ch_;
    dsp::Lfo lfo_;
    int controlCounter_ = 0;
};

// Rotating speaker ("slow" chorale or "fast" tremolo): crossover at 800 Hz,
// horn and drum with their own rotation, Doppler (modulated delay),
// amplitude modulation, and the horn getting darker when it points away;
// two virtual microphones around the cabinet.
// "cabinet" adds what the microphones hear besides the direct sound: the
// rotors bounce off the cabinet walls, the floor and the room, and every
// reflection sees the rotor from another angle. That is what makes a
// recorded Leslie wide and swirling instead of a left-right ping-pong.
// "top_hz" is the treble limit of the horn and the recording.
class Leslie : public Effect {
public:
    explicit Leslie(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;
    // "speed" can be switched: the rotors speed up and slow down like a real cabinet.
    bool configureSwitch(const std::string& param, const nlohmann::json& a, const nlohmann::json& b) override;
    void setSwitch(bool positionB, bool immediate) override;

private:
    static constexpr int kReflections = 7;
    struct Reflection {
        float delay[2];          // samples, per microphone
        float cosA[2], sinA[2];  // angle at which this reflection sees the rotor
        float gain;
    };
    void setRates(bool fast, bool immediate);
    float reflections(const dsp::DelayLine& line, float base, float doppler, float am, float s, float c,
                      float direction, int mic) const;

    bool fast_;
    bool switchable_ = false;
    bool fastA_ = false, fastB_ = true;
    float hornHz_ = 0.0f, drumHz_ = 0.0f, hornTarget_ = 0.0f, drumTarget_ = 0.0f;
    float hornUp_ = 0.0f, hornDown_ = 0.0f, drumUp_ = 0.0f, drumDown_ = 0.0f;  // per-sample coefficients
    float depth_, spread_, drive_, mix_, cabinet_, topHz_;
    double sampleRate_ = 48000.0;
    dsp::Biquad lowA_, lowB_, highA_, highB_;  // Linkwitz-Riley 4th order
    dsp::DelayLine horn_, drum_;
    dsp::Lfo hornLfo_, drumLfo_;
    float hornDoppler_ = 0.0f, drumDoppler_ = 0.0f, hornBase_ = 0.0f, drumBase_ = 0.0f;
    float hornAm_ = 0.0f, drumAm_ = 0.0f, hornMakeup_ = 1.0f, drumMakeup_ = 1.0f;
    float darkCoef_ = 1.0f, brightCoef_ = 1.0f;  // horn low-pass facing away / facing the mic
    std::array<float, 2> hornTone_{};             // low-pass state per microphone
    std::array<Reflection, kReflections> refl_{};
    float reflCoef_ = 1.0f, cabinetMakeup_ = 1.0f;
    std::array<float, 2> hornReflTone_{}, drumReflTone_{};  // walls absorb the treble
    std::array<dsp::Biquad, 2> top_;
    bool topOn_ = false;
};

// Small room: a decorrelated stereo reverb (feedback delay network, 8 lines)
// added to the dry sound. Gives space and width to a mono source; with a
// Leslie, it is the room around the cabinet.
class Room : public Effect {
public:
    explicit Room(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    static constexpr int kLines = 8;
    float level_, timeS_, dampHz_, predelayMs_, lowCutHz_;
    float predelay_ = 0.0f;
    dsp::DelayLine pre_;
    dsp::Biquad lowCut_;
    std::array<dsp::DelayLine, kLines> lines_;
    std::array<float, kLines> length_{}, gain_{}, damp_{};
    float dampCoef_ = 1.0f, outGain_ = 1.0f;
};

// Fuzz with octave, in the style of the late-60s octave fuzz pedals:
// heavy clipping with an octave-up generator (full-wave rectification), a
// blend between clean and fuzz, and a tone control. Mono, like the pedal;
// runs at 4x the sample rate so that hard clipping does not alias.
class Fuzz : public Effect {
public:
    explicit Fuzz(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    static constexpr int kOversample = 4;
    static constexpr int kFilterStages = 4;  // 8th-order Butterworth around the clipper
    float drive_, octave_, bias_, toneHz_, lowCutHz_, blend_, level_;
    float preGain_ = 1.0f, biasOffset_ = 0.0f;
    dsp::Biquad lowCut_, octaveDc_, dcBlock_, tone_;
    std::array<dsp::Biquad, kFilterStages> up_, down_;
};

// Stereo width for mono sounds. A delayed, bass-free copy of the centre is
// added to the left and subtracted from the right: the sound opens up, while
// L + R stays exactly the dry signal, so nothing changes on a mono PA.
class Width : public Effect {
public:
    explicit Width(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    float amount_, timeMs_, lowCutHz_;
    float delaySamples_ = 0.0f;
    dsp::DelayLine line_;
    dsp::Biquad lowCut_;
};

// Tremolo (amplitude modulation); "stereo" makes it a pan tremolo.
class Tremolo : public Effect {
public:
    explicit Tremolo(FxParams& p);
    void prepare(double sampleRate) override;
    void reset() override;
    void process(float* left, float* right, int frames) override;

private:
    float rateHz_, depth_;
    int shape_;  // 0 sine, 1 triangle, 2 square (rounded)
    bool stereo_;
    float makeup_ = 1.0f;
    dsp::Lfo lfo_;
};

} // namespace mbx
