#pragma once

#include "core/Constants.h"
#include "core/EngineEvent.h"
#include "core/Limiter.h"
#include "core/SpscQueue.h"
#include "sampler/Program.h"
#include "sampler/VoicePool.h"

#include <array>
#include <atomic>
#include <vector>

namespace mbx {

// The orchestrator. render() runs in the audio thread; everything else
// talks to it through lock-free queues, one per producer thread ("source").
//
// Threading rules:
//  - prepare(), setPolyphony(), setMidiChannel(), setPresets(),
//    selectPresetNow(), setProgram(), setMasterGainDb(): before the audio starts;
//  - registerSource(), releaseSource(): any non-audio thread, any time;
//  - post(source, ...): only from the thread that owns that source;
//  - render(): audio thread only.
class FxChain;
class Recorder;

class Engine {
public:
    static constexpr int kMaxSources = 16;
    static constexpr std::size_t kQueueSize = 1024;

    void prepare(double sampleRate);
    void setPolyphony(int voices) { voices_.setPolyphony(voices); }
    int polyphony() const { return voices_.polyphony(); }
    // 1..16, or 0 to accept every channel (omni).
    void setMidiChannel(int channel) { midiChannel_ = channel; }
    int midiChannel() const { return midiChannel_; }
    // The table must outlive the engine and never change while it runs.
    void setPresets(const PresetTable* presets);
    bool selectPresetNow(int slot);              // 1..128; false if the slot is empty
    void setProgram(const Program* program);     // a program outside the table (test mode)
    void setMasterGainDb(float db);

    int registerSource();          // -1 when all sources are taken
    void releaseSource(int source);
    bool post(int source, const EngineEvent& event);

    void render(float* interleavedStereo, int frames);

    // Recording (any non-audio thread): the audio thread copies its output into
    // the recorder from the next block on. After setRecorder(nullptr), call
    // waitForRender() before destroying the old recorder.
    void setRecorder(Recorder* recorder) { recorder_.store(recorder, std::memory_order_release); }
    void waitForRender() const;

    double sampleRate() const { return sampleRate_; }
    int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
    int droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }
    // Preset now playing (1..128), 0 when the program did not come from the table.
    int currentSlot() const { return currentSlot_.load(std::memory_order_relaxed); }
    // Position of the program's effect switch: false = A (as selected), true = B.
    bool fxSwitchB() const { return fxSwitchState_.load(std::memory_order_relaxed); }
    // Output of the program now playing: -1 = none, else int(Output) (A, B, AB).
    int currentOutput() const { return output_.load(std::memory_order_relaxed); }
    // Last SwapPresets generation applied by the audio thread: the tables of
    // older generations are no longer used (after kFxFadeMs).
    int appliedGeneration() const { return appliedGeneration_.load(std::memory_order_acquire); }
    // MIDI messages received, any channel (for an activity indicator).
    unsigned midiMessages() const { return midiMessages_.load(std::memory_order_relaxed); }
    float masterGainDb() const { return masterGainDb_.load(std::memory_order_relaxed); }

    // Output levels since the previous call (main thread, for --stats).
    struct LevelStats {
        float peak = 0.0f;            // highest level before the limiter, 1.0 = 0 dBFS
        float minLimiterGain = 1.0f;  // strongest limiter action, 1.0 = never active
        int maxVoices = 0;            // most voices sounding at the same time
    };
    LevelStats takeLevelStats();

private:
    using Queue = SpscQueue<EngineEvent, kQueueSize>;

    void processEvents();
    void handle(const EngineEvent& event);
    void handleMidi(const MidiEvent& m);
    void noteOn(int key, int velocity);
    void noteOff(int key);
    void setSustain(bool down);
    void releaseAll();
    void killAll();
    void allSoundOff();  // notes cut and effect tails (echo...) silenced
    void selectPreset(int slot);
    void switchProgram(const Program* next);
    void setFxSwitch(bool positionB, bool immediate);
    void finishFade();
    void swapPresets(const PresetTable* table, int generation);
    void useChains(const Program* program);
    void setCurrent(const Program* program);  // program_ and the output it plays on  // registers and prepares a program's chain (not real-time)

    double sampleRate_ = 48000.0;
    const Program* program_ = nullptr;
    const PresetTable* presets_ = nullptr;
    VoicePool voices_;
    bool sustain_ = false;
    int midiChannel_ = 0;

    float gain_ = 1.0f;        // smoothed master gain
    float gainTarget_ = 1.0f;
    float gainCoef_ = 0.0f;
    Limiter limiter_;

    std::array<Queue, kMaxSources> queues_;
    std::array<std::atomic<bool>, kMaxSources> inUse_{};

    // Effects: two buses, each with the chain of a program. New notes play into
    // the active bus; on a program change the old bus fades out (kFxFadeMs) with
    // its chain, so echo tails and modulation stop without a click.
    std::array<FxChain*, 2> busChain_{};
    int activeBus_ = 0;
    int fadeRemaining_ = 0;   // samples left of the fade of the other bus
    int fadeTotal_ = 1;
    int flushRemaining_ = 0;  // all sound off: samples left of the fade of the active effects
    // Live edit: the program's old chain fades out while the new one fades in,
    // on the same voices (no note is cut).
    FxChain* oldChain_ = nullptr;
    int chainFade_ = 0;
    std::vector<FxChain*> chains_;  // every chain the engine may use, to prepare them
    bool prepared_ = false;

    std::array<float, kMaxBlockFrames> left_{};
    std::array<float, kMaxBlockFrames> right_{};
    std::array<float, kMaxBlockFrames> fadeLeft_{};
    std::array<float, kMaxBlockFrames> fadeRight_{};
    std::array<float, kMaxBlockFrames> chainLeft_{};
    std::array<float, kMaxBlockFrames> chainRight_{};
    std::atomic<int> appliedGeneration_{0};
    std::atomic<Recorder*> recorder_{nullptr};
    std::atomic<std::uint64_t> renderCount_{0};
    std::atomic<unsigned> midiMessages_{0};
    std::atomic<float> masterGainDb_{0.0f};

    std::atomic<int> activeVoices_{0};
    std::atomic<int> dropped_{0};
    std::atomic<int> currentSlot_{0};
    std::atomic<int> output_{-1};
    std::atomic<bool> fxSwitchState_{false};
    bool fxSwitch_ = false;
    std::atomic<float> peak_{0.0f};
    std::atomic<int> maxVoices_{0};
    std::atomic<float> minLimiterGain_{1.0f};
};

} // namespace mbx
