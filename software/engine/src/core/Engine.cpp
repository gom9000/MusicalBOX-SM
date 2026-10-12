#include "core/Engine.h"

#include "core/Recorder.h"
#include "fx/Effects.h"
#include "fx/FxChain.h"
#include "sampler/Instrument.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <thread>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace mbx {

void Engine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    gainCoef_ = 1.0f - static_cast<float>(std::exp(-1.0 / (kGainSmoothingMs * 0.001 * sampleRate)));
    limiter_.prepare(sampleRate, kLimiterThresholdDb, kLimiterReleaseMs);
    fadeTotal_ = std::max(1, static_cast<int>(kFxFadeMs * 0.001 * sampleRate));
    prepared_ = true;
    for (FxChain* chain : chains_) chain->prepare(sampleRate);
}

void Engine::useChains(const Program* program) {
    if (!program || !program->fxChain) return;
    FxChain* chain = program->fxChain.get();
    if (std::find(chains_.begin(), chains_.end(), chain) == chains_.end()) chains_.push_back(chain);
    if (prepared_ && !chain->prepared()) chain->prepare(sampleRate_);
}

Engine::LevelStats Engine::takeLevelStats() {
    LevelStats s;
    s.peak = peak_.exchange(0.0f, std::memory_order_relaxed);
    s.minLimiterGain = minLimiterGain_.exchange(1.0f, std::memory_order_relaxed);
    s.maxVoices = maxVoices_.exchange(activeVoices_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return s;
}

void Engine::waitForRender() const {
    // Two complete blocks after the change: no render can still hold the old pointer.
    // If the audio is not running there is nothing to wait for (bounded wait).
    const std::uint64_t start = renderCount_.load(std::memory_order_acquire);
    for (int i = 0; i < 100 && renderCount_.load(std::memory_order_acquire) < start + 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void Engine::setPresets(const PresetTable* presets) {
    presets_ = presets;
    if (presets)
        for (const Program* p : *presets) useChains(p);
}

bool Engine::selectPresetNow(int slot) {
    if (!presets_ || slot < 1 || slot > kPresetCount || !(*presets_)[slot - 1]) return false;
    setProgram((*presets_)[slot - 1]);
    currentSlot_.store(slot, std::memory_order_relaxed);
    return true;
}

void Engine::setProgram(const Program* program) {
    useChains(program);
    setCurrent(program);
    busChain_[activeBus_] = program && program->fxChain ? program->fxChain.get() : nullptr;
    if (FxChain* chain = busChain_[activeBus_]) {
        chain->reset();
        chain->restore();
        chain->setSwitch(false, true);
    }
    fxSwitch_ = false;
    fxSwitchState_.store(false, std::memory_order_relaxed);
    currentSlot_.store(0, std::memory_order_relaxed);
}

void Engine::setCurrent(const Program* program) {
    program_ = program;
    output_.store(program ? static_cast<int>(program->output) : -1, std::memory_order_relaxed);
}

void Engine::setMasterGainDb(float db) {
    masterGainDb_.store(db, std::memory_order_relaxed);
    gainTarget_ = dbToGain(db);
    gain_ = gainTarget_;
}

int Engine::registerSource() {
    for (int i = 0; i < kMaxSources; ++i) {
        bool expected = false;
        if (inUse_[i].compare_exchange_strong(expected, true)) return i;
    }
    return -1;
}

void Engine::releaseSource(int source) {
    if (source >= 0 && source < kMaxSources) inUse_[source].store(false);
}

bool Engine::post(int source, const EngineEvent& event) {
    if (source < 0 || source >= kMaxSources) return false;
    if (queues_[source].push(event)) return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

namespace {
// Very small numbers ("denormals") in filter and echo tails make the CPU much
// slower on some processors: flush them to zero in the audio thread.
void flushDenormals() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
#if defined(__aarch64__)
    std::uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);  // FZ
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__SSE__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ + DAZ
#endif
}
} // namespace

void Engine::render(float* out, int frames) {
    flushDenormals();
    processEvents();

    float blockPeak = 0.0f;
    float blockMinGain = 1.0f;
    int done = 0;
    while (done < frames) {
        const int n = std::min(frames - done, kMaxBlockFrames);
        std::fill_n(left_.data(), n, 0.0f);
        std::fill_n(right_.data(), n, 0.0f);

        // Speed of the voices in this block: pitch bend, and the motor of the
        // program ("motor" in fx), each as a ramp from the previous block.
        const float bendFrom = bend_;
        bend_ = bendTarget_;
        auto motorMod = [&](FxChain* chain, bool advance) {
            VoiceMod mod{bendFrom, bend_, 1.0f, 1.0f};
            if (Motor* motor = chain ? chain->motor() : nullptr) {
                const float from = motor->speed();
                const float to = advance ? motor->advance(n) : from;
                mod = VoiceMod{bendFrom * from, bend_ * to, from, to};
            }
            return mod;
        };

        // Active program: its voices through its effects.
        voices_.render(activeBus_, left_.data(), right_.data(), n, motorMod(busChain_[activeBus_], true));
        const bool chainFading = chainFade_ > 0 && oldChain_;
        if (chainFading) {
            std::copy_n(left_.data(), n, chainLeft_.data());
            std::copy_n(right_.data(), n, chainRight_.data());
            oldChain_->process(chainLeft_.data(), chainRight_.data(), n);
        }
        if (FxChain* chain = busChain_[activeBus_]) {
            chain->process(left_.data(), right_.data(), n);
            if (flushRemaining_ > 0) {
                // All sound off: fade the effect tails out, then clear them.
                const float step = 1.0f / static_cast<float>(fadeTotal_);
                float g = static_cast<float>(flushRemaining_) * step;
                for (int i = 0; i < n; ++i) {
                    g = std::max(0.0f, g - step);
                    left_[i] *= g;
                    right_[i] *= g;
                }
                flushRemaining_ = std::max(0, flushRemaining_ - n);
                if (flushRemaining_ == 0) {
                    chain->reset();
                    chain->setSwitch(fxSwitch_, true);  // keep the switch where it was
                }
            }
        }

        if (chainFading) {
            const float step = 1.0f / static_cast<float>(fadeTotal_);
            float g = static_cast<float>(chainFade_) * step;  // weight of the old chain
            for (int i = 0; i < n; ++i) {
                left_[i] += (chainLeft_[i] - left_[i]) * g;
                right_[i] += (chainRight_[i] - right_[i]) * g;
                g = std::max(0.0f, g - step);
            }
            chainFade_ = std::max(0, chainFade_ - n);
            if (chainFade_ == 0) oldChain_ = nullptr;
        }

        // Previous program, while it fades out after a program change. Voices
        // left on that bus are rendered anyway, so their slots are freed.
        const int other = activeBus_ ^ 1;
        const bool fading = fadeRemaining_ > 0;
        if (fading || voices_.busActive(other)) {
            std::fill_n(fadeLeft_.data(), n, 0.0f);
            std::fill_n(fadeRight_.data(), n, 0.0f);
            voices_.render(other, fadeLeft_.data(), fadeRight_.data(), n, motorMod(busChain_[other], false));
            if (fading) {
                if (FxChain* chain = busChain_[other]) chain->process(fadeLeft_.data(), fadeRight_.data(), n);
                const float step = 1.0f / static_cast<float>(fadeTotal_);
                float g = static_cast<float>(fadeRemaining_) * step;
                for (int i = 0; i < n; ++i) {
                    left_[i] += fadeLeft_[i] * g;
                    right_[i] += fadeRight_[i] * g;
                    g = std::max(0.0f, g - step);
                }
                fadeRemaining_ = std::max(0, fadeRemaining_ - n);
                if (fadeRemaining_ == 0) finishFade();
            }
        }

        float* o = out + 2 * done;
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) {
            gain_ += (gainTarget_ - gain_) * gainCoef_;
            o[2 * i] = left_[i] * gain_;
            o[2 * i + 1] = right_[i] * gain_;
            peak = std::max(peak, std::max(std::fabs(o[2 * i]), std::fabs(o[2 * i + 1])));
        }
        const float limiterGain = limiter_.process(o, n);
        for (int i = 0; i < 2 * n; ++i) o[i] = std::clamp(o[i], -1.0f, 1.0f);  // safety only
        if (Recorder* rec = recorder_.load(std::memory_order_acquire)) rec->push(o, n);
        blockPeak = std::max(blockPeak, peak);
        blockMinGain = std::min(blockMinGain, limiterGain);
        done += n;
    }

    // Level statistics for --stats (read and reset by the main thread).
    float previous = peak_.load(std::memory_order_relaxed);
    while (blockPeak > previous && !peak_.compare_exchange_weak(previous, blockPeak, std::memory_order_relaxed)) {}
    previous = minLimiterGain_.load(std::memory_order_relaxed);
    while (blockMinGain < previous &&
           !minLimiterGain_.compare_exchange_weak(previous, blockMinGain, std::memory_order_relaxed)) {}

    renderCount_.fetch_add(1, std::memory_order_release);
    const int active = voices_.activeCount();
    activeVoices_.store(active, std::memory_order_relaxed);
    int previousMax = maxVoices_.load(std::memory_order_relaxed);
    while (active > previousMax && !maxVoices_.compare_exchange_weak(previousMax, active, std::memory_order_relaxed)) {}
}

void Engine::processEvents() {
    EngineEvent event;
    for (auto& queue : queues_)
        while (queue.pop(event)) handle(event);
}

void Engine::handle(const EngineEvent& event) {
    switch (event.type) {
    case EngineEvent::Type::Midi:
        handleMidi(event.midi);
        break;
    case EngineEvent::Type::MasterGainDb:
        masterGainDb_.store(event.value, std::memory_order_relaxed);
        gainTarget_ = dbToGain(event.value);
        break;
    case EngineEvent::Type::AllSoundOff:
        allSoundOff();
        break;
    case EngineEvent::Type::SelectPreset:
        selectPreset(event.slot);
        break;
    case EngineEvent::Type::SwapPresets:
        swapPresets(event.presets, event.slot);
        break;
    case EngineEvent::Type::FxSwitch:
        if (busChain_[activeBus_] && busChain_[activeBus_]->hasSwitch())
            setFxSwitch(event.slot < 0 ? !fxSwitch_ : event.slot == 1, false);
        break;
    case EngineEvent::Type::Fx:
        if (event.chain) event.chain->apply(event.fx, event.slot == 1);
        break;
    case EngineEvent::Type::FxCurrent:
        if (event.chain && event.chain == busChain_[activeBus_]) event.chain->apply(event.fx, false);
        break;
    }
}

void Engine::handleMidi(const MidiEvent& m) {
    midiMessages_.fetch_add(1, std::memory_order_relaxed);
    if (midiChannel_ != 0 && m.channel() != midiChannel_) return;
    switch (m.command()) {
    case midi::kNoteOn:
        if (m.data2 > 0)
            noteOn(m.data1, m.data2);
        else
            noteOff(m.data1);
        break;
    case midi::kNoteOff:
        noteOff(m.data1);
        break;
    case midi::kControlChange:
        if (m.data1 == midi::kCcSustain)
            setSustain(m.data2 >= 64);
        else if (m.data1 == midi::kCcAllSoundOff)
            allSoundOff();
        else if (m.data1 == midi::kCcAllNotesOff)
            releaseAll();
        else if (FxChain* chain = busChain_[activeBus_])
            chain->midiControl(m.data1, m.data2);  // parameters mapped with "cc"
        break;
    case midi::kPitchBend:
        if (bendRange_ > 0.0f) {
            const int value = (m.data2 << 7) | m.data1;  // 0..16383, 8192 = centre
            const float semitones = static_cast<float>(value - 8192) / 8192.0f * bendRange_;
            bendTarget_ = std::exp2(semitones / 12.0f);
        }
        break;
    case midi::kProgramChange:
        // Program Change 0..127 selects preset 1..128 (an undefined one is ignored).
        selectPreset(m.data1 + 1);
        break;
    default:
        break;
    }
}

void Engine::noteOn(int key, int velocity) {
    if (!program_) return;
    for (const Layer& layer : program_->layers) {
        if (!layer.instrument || key < layer.keyLo || key > layer.keyHi) continue;
        const int played = std::clamp(key + layer.transpose, 0, 127);
        const auto mapped = layer.velocity.map(velocity);
        const Zone* zone = layer.instrument->findZone(played, mapped.lookupVelocity);
        if (!zone) continue;
        Voice* voice = voices_.allocate(sampleRate_);
        if (!voice) continue;
        const float gain = program_->gain * layer.gain * mapped.gain;
        voice->start(*zone, layer, key, played, gain, sampleRate_, voices_.nextOrder());
        voice->bus = activeBus_;
    }
}

void Engine::noteOff(int key) {
    voices_.forEachActive([&](Voice& v) {
        if (v.key() != key || v.released() || v.killed() || v.sustained) return;
        if (sustain_)
            v.sustained = true;
        else
            v.release();
    });
}

void Engine::setSustain(bool down) {
    sustain_ = down;
    if (down) return;
    voices_.forEachActive([](Voice& v) {
        if (v.sustained) {
            v.sustained = false;
            v.release();
        }
    });
}

void Engine::releaseAll() {
    sustain_ = false;
    voices_.forEachActive([](Voice& v) {
        v.sustained = false;
        v.release();
    });
}

// Switches program: the notes of the old one are cut with a short fade, the
// pedal state is cleared. An empty slot, or the slot already playing, is
// ignored (a double press must not cut a held chord).
void Engine::selectPreset(int slot) {
    if (!presets_ || slot < 1 || slot > kPresetCount) return;
    const Program* next = (*presets_)[slot - 1];
    if (!next || slot == currentSlot_.load(std::memory_order_relaxed)) return;
    switchProgram(next);
    currentSlot_.store(slot, std::memory_order_relaxed);
}

// Audio thread. The old program's notes are cut (5 ms) and its effects fade
// out on the other bus; the new program starts on a clean chain.
void Engine::switchProgram(const Program* next) {
    killAll();
    if (next == program_) return;  // same program on another button: keep its effects running
    oldChain_ = nullptr;  // a live-edit cross-fade still running: the program is leaving anyway
    chainFade_ = 0;
    if (fadeRemaining_ > 0) finishFade();  // a fade still running: cut it short

    FxChain* chain = next->fxChain ? next->fxChain.get() : nullptr;
    activeBus_ ^= 1;
    if (chain && chain == busChain_[activeBus_ ^ 1]) chain = nullptr;  // never one chain on both buses
    busChain_[activeBus_] = chain;
    fadeRemaining_ = fadeTotal_;
    setCurrent(next);
    flushRemaining_ = 0;
    if (chain) chain->restore();  // controllers moved last time: back to the programmed values
    setFxSwitch(false, true);     // every program starts from its A values
}

// Audio thread. New preset table: the program playing is looked up by id
// (same button first). If it is still there, its notes go on and its effects
// cross-fade from the old chain to the new one; otherwise the button's new
// program (or the first one) is selected as on a program change.
void Engine::swapPresets(const PresetTable* table, int generation) {
    if (!table) return;
    presets_ = table;
    const int slot = currentSlot_.load(std::memory_order_relaxed);
    const Program* next = nullptr;
    int nextSlot = 0;
    if (program_) {
        if (slot >= 1 && (*table)[slot - 1] && (*table)[slot - 1]->id == program_->id) {
            next = (*table)[slot - 1];
            nextSlot = slot;
        }
        for (int i = 0; !next && i < kPresetCount; ++i)
            if ((*table)[i] && (*table)[i]->id == program_->id) next = (*table)[i], nextSlot = i + 1;
    }
    const bool sameProgram = next != nullptr;
    if (!next && slot >= 1 && (*table)[slot - 1]) next = (*table)[slot - 1], nextSlot = slot;
    for (int i = 0; !next && i < kPresetCount; ++i)
        if ((*table)[i]) next = (*table)[i], nextSlot = i + 1;

    if (!next) {
        killAll();
        setCurrent(nullptr);
    } else if (!sameProgram) {
        switchProgram(next);
    } else {
        setCurrent(next);
        FxChain* chain = next->fxChain ? next->fxChain.get() : nullptr;
        if (chain && chain == busChain_[activeBus_]) {
            // Same effects, new values (sent as Fx events): nothing to swap, the tails go on.
            currentSlot_.store(nextSlot, std::memory_order_relaxed);
            appliedGeneration_.store(generation, std::memory_order_release);
            return;
        }
        oldChain_ = busChain_[activeBus_];
        chainFade_ = oldChain_ ? fadeTotal_ : 0;
        busChain_[activeBus_] = chain;
        flushRemaining_ = 0;
        if (chain) {
            chain->reset();
            if (chain->hasSwitch())
                chain->setSwitch(fxSwitch_, true);
            else
                setFxSwitch(false, true);
        } else {
            setFxSwitch(false, true);
        }
    }
    currentSlot_.store(nextSlot, std::memory_order_relaxed);
    appliedGeneration_.store(generation, std::memory_order_release);
}

void Engine::setFxSwitch(bool positionB, bool immediate) {
    fxSwitch_ = positionB;
    fxSwitchState_.store(positionB, std::memory_order_relaxed);
    if (FxChain* chain = busChain_[activeBus_]) chain->setSwitch(positionB, immediate);
}

void Engine::finishFade() {
    const int other = activeBus_ ^ 1;
    if (FxChain* chain = busChain_[other]) chain->reset();  // clean for the next time it is selected
    busChain_[other] = nullptr;
    fadeRemaining_ = 0;
}

void Engine::allSoundOff() {
    killAll();
    if (busChain_[activeBus_]) flushRemaining_ = fadeTotal_;
}

void Engine::killAll() {
    sustain_ = false;
    voices_.forEachActive([&](Voice& v) {
        v.sustained = false;
        v.kill(sampleRate_);
    });
}

} // namespace mbx
