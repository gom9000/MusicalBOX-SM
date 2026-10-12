#pragma once

// The effects of MusicalBOX SM. The parameters of each type, with defaults
// and ranges, are in Effect.cpp (and in README, "Effects").

#include "fx/Dsp.h"
#include "fx/Effect.h"

#include <array>

namespace mbx {

// Equaliser: low cut, bass shelf, mid peak, treble shelf, high cut, output gain.
class Tone : public Effect {
public:
    enum { kLowCut, kBassDb, kBassHz, kMidDb, kMidHz, kTrebleDb, kTrebleHz, kHighCut, kGain };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
    float gain_ = 1.0f;
    struct Channel {
        dsp::Biquad lowCut, bass, mid, treble, highCut;
    };
    std::array<Channel, 2> ch_;
};

// Tape echo: one playback head, feedback through a darkening, saturating loop,
// slight wow and flutter. Optional ping-pong between left and right.
class TapeEcho : public Effect {
public:
    enum { kTime, kFeedback, kMix, kTone, kLowCut, kWow, kDrive, kPingPong };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
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
    enum { kMode, kRate, kDepth, kMix };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
    bool ensemble_ = false;
    float baseDelay_ = 0.0f, slowDepth_ = 0.0f, fastDepth_ = 0.0f, makeup_ = 1.0f;
    std::array<dsp::DelayLine, 2> line_;
    std::array<dsp::OnePole, 2> dark_;
    dsp::Lfo slow_, fast_;
};

// Phaser, the classic four-stage pedal: four all-pass stages swept by an
// LFO, mixed 50/50 with the dry signal, with optional feedback.
class Phaser : public Effect {
public:
    enum { kRate, kDepth, kFeedback, kMix, kStages, kStereo };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
    static constexpr int kMaxStages = 12;
    int stages_ = 4;
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
// recorded rotating speaker wide and swirling instead of a left-right ping-pong.
// "top_hz" is the treble limit of the horn and the recording.
// A speed change makes the rotors speed up and slow down like a real cabinet.
class Leslie : public Effect {
public:
    enum { kSpeed, kDepth, kSpread, kDrive, kMix, kCabinet, kTop };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

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

    float hornHz_ = 0.0f, drumHz_ = 0.0f, hornTarget_ = 0.0f, drumTarget_ = 0.0f;
    float hornUp_ = 0.0f, hornDown_ = 0.0f, drumUp_ = 0.0f, drumDown_ = 0.0f;  // per-sample coefficients
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
// rotating speaker, it is the room around the cabinet.
class Room : public Effect {
public:
    enum { kLevel, kTime, kDamp, kPredelay, kLowCut };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
    static constexpr int kLines = 8;
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
    enum { kDrive, kOctave, kBias, kTone, kLowCut, kBlend, kLevel };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
    static constexpr int kOversample = 4;
    static constexpr int kFilterStages = 4;  // 8th-order Butterworth around the clipper
    float preGain_ = 1.0f, biasOffset_ = 0.0f, level_ = 1.0f;
    dsp::Biquad lowCut_, octaveDc_, dcBlock_, tone_;
    std::array<dsp::Biquad, kFilterStages> up_, down_;
};

// Stereo width for mono sounds. A delayed, bass-free copy of the centre is
// added to the left and subtracted from the right: the sound opens up, while
// L + R stays exactly the dry signal, so nothing changes on a mono PA.
class Width : public Effect {
public:
    enum { kAmount, kTime, kLowCut };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
    float delaySamples_ = 0.0f;
    dsp::DelayLine line_;
    dsp::Biquad lowCut_;
};

// Tremolo (amplitude modulation); "stereo" makes it a pan tremolo.
class Tremolo : public Effect {
public:
    enum { kRate, kDepth, kShape, kStereo };
    void reset() override;

protected:
    void allocate(double sampleRate) override;
    void update(bool immediate) override;
    void process(float* left, float* right, int frames) override;

private:
    float makeup_ = 1.0f;
    dsp::Lfo lfo_;
};

// The motor of an electromechanical organ: not an audio effect but the speed
// of the program's voices. "off" slows every note down to silence (pitch and
// level fall together, as tonewheels and their pickups), "on" brings them back
// up, each move starting from the current speed. The engine reads speed().
class Motor : public Effect {
public:
    enum { kState, kStop, kStart };
    void reset() override;

    float speed() const { return speed_; }  // 0 = stopped, 1 = running
    float advance(int frames);              // audio thread: moves towards the target, returns the new speed

protected:
    void allocate(double) override {}
    void update(bool immediate) override;
    void process(float*, float*, int) override {}  // the sound passes unchanged

private:
    bool running_ = true;
    float speed_ = 1.0f;
    float down_ = 0.0f, up_ = 0.0f;  // speed change per sample
};

} // namespace mbx
