#include "audio/AudioDevice.h"

#include "core/Engine.h"
#include "core/Log.h"

#include "miniaudio/miniaudio.h"

#ifdef __linux__
#include <alsa/asoundlib.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>

namespace mbx {

namespace {

// On Linux talk to ALSA directly: a sound server (PulseAudio, PipeWire) adds
// latency and hides the real cards. The others are only fallbacks.
#ifdef __linux__
const ma_backend kBackends[] = {ma_backend_alsa, ma_backend_pulseaudio, ma_backend_jack};
const ma_uint32 kBackendCount = 3;
#else
const ma_backend* const kBackends = nullptr;
const ma_uint32 kBackendCount = 0;
#endif

#ifdef __linux__
void silentAlsaError(const char*, int, const char*, int, const char*, ...) {}

// Mutes ALSA's messages on stderr for the lifetime of the object.
struct QuietAlsa {
    QuietAlsa() { snd_lib_error_set_handler(&silentAlsaError); }
    ~QuietAlsa() { snd_lib_error_set_handler(nullptr); }
};
#else
struct QuietAlsa {};
#endif

} // namespace

struct AudioDevice::Impl {
    ma_context context{};
    ma_device device{};
    bool contextReady = false;
    bool deviceReady = false;
    std::string chosenName;  // name from the device list (miniaudio cannot read it back once the card is held)
    Engine* engine = nullptr;
    std::atomic<float> maxLoad{0.0f};
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> busyNs{0};
    std::atomic<std::uint64_t> frames{0};

    static void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(device->pUserData);
        const auto t0 = std::chrono::steady_clock::now();

        self->engine->render(static_cast<float*>(output), static_cast<int>(frames));

        const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        const float budget = static_cast<float>(frames) / static_cast<float>(device->sampleRate);
        const float load = elapsed / budget;
        float previous = self->maxLoad.load(std::memory_order_relaxed);
        while (load > previous &&
               !self->maxLoad.compare_exchange_weak(previous, load, std::memory_order_relaxed)) {
        }
        self->callbacks.fetch_add(1, std::memory_order_relaxed);
        self->busyNs.fetch_add(static_cast<std::uint64_t>(elapsed * 1e9f), std::memory_order_relaxed);
        self->frames.fetch_add(frames, std::memory_order_relaxed);
    }

    // nullBackend: no sound card, the engine runs on a timer (tests, --device null).
    bool initContext(std::string& error, bool nullBackend = false) {
        if (contextReady) return true;
        ma_context_config config = ma_context_config_init();
        config.threadPriority = ma_thread_priority_realtime;
        const ma_backend nullOnly[] = {ma_backend_null};
        if (ma_context_init(nullBackend ? nullOnly : kBackends, nullBackend ? 1 : kBackendCount, &config, &context) !=
            MA_SUCCESS) {
            error = "cannot initialise the audio backend";
            return false;
        }
        contextReady = true;
        return true;
    }
};

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
    stop();
    if (impl_->deviceReady) ma_device_uninit(&impl_->device);
    if (impl_->contextReady) ma_context_uninit(&impl_->context);
}

bool AudioDevice::open(Engine& engine, const AudioOptions& options, std::string& error) {
    const bool nullBackend = options.deviceName == "null";
    if (!impl_->initContext(error, nullBackend)) return false;
    impl_->engine = &engine;

    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    const ma_device_id* chosen = nullptr;
    if (!options.deviceName.empty() && !nullBackend) {
        if (ma_context_get_devices(&impl_->context, &playback, &playbackCount, nullptr, nullptr) != MA_SUCCESS) {
            error = "cannot list audio devices";
            return false;
        }
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string wanted = lower(options.deviceName);
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            if (lower(playback[i].name).find(wanted) != std::string::npos) {
                chosen = &playback[i].id;
                impl_->chosenName = playback[i].name;
                break;
            }
        }
        if (!chosen) {
            error = "no output device matches \"" + options.deviceName + "\" (see --list-devices)";
            return false;
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = chosen;
    // Exclusive: on ALSA this opens the card directly ("hw"). In shared mode
    // miniaudio goes through "dmix", which imposes its own large period
    // (1024 frames, about 64 ms of buffer) whatever we ask for.
    config.playback.shareMode = ma_share_mode_exclusive;
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = options.sampleRate;
    config.periodSizeInFrames = options.periodFrames;
    config.periods = options.periods;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE;  // render() writes every frame
    config.dataCallback = &Impl::dataCallback;
    config.pUserData = impl_.get();

    // While the card is held in direct mode, miniaudio probes it again through
    // "dmix" to read its name, and ALSA prints "unable to open slave": harmless, hidden.
    QuietAlsa quiet;
    if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS) {
        // The card may be busy (another program, a sound server): share it.
        config.playback.shareMode = ma_share_mode_shared;
        if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS) {
            error = "cannot open the audio device";
            return false;
        }
        log::warn("audio: the card is in use by another program, opened in shared mode (more latency)");
    }
    if (impl_->device.playback.internalPeriodSizeInFrames > options.periodFrames)
        log::warn("audio: asked for a period of %u frames, the card gave %u", options.periodFrames,
                  impl_->device.playback.internalPeriodSizeInFrames);
    impl_->deviceReady = true;
    engine.prepare(impl_->device.sampleRate);
    return true;
}

bool AudioDevice::start(std::string& error) {
    if (!impl_->deviceReady) {
        error = "device not open";
        return false;
    }
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        error = "cannot start the audio device";
        return false;
    }
    return true;
}

void AudioDevice::stop() {
    if (impl_->deviceReady && ma_device_is_started(&impl_->device)) ma_device_stop(&impl_->device);
}

std::string AudioDevice::deviceName() const {
    if (!impl_->deviceReady) return {};
    const bool exclusive = impl_->device.playback.shareMode == ma_share_mode_exclusive;
    const std::string name = impl_->chosenName.empty() ? impl_->device.playback.name : impl_->chosenName;
    return name + " (" + ma_get_backend_name(impl_->context.backend) +
           (exclusive ? ", direct" : ", shared") + ")";
}

std::uint32_t AudioDevice::sampleRate() const {
    return impl_->deviceReady ? impl_->device.sampleRate : 0;
}

std::uint32_t AudioDevice::periodFrames() const {
    return impl_->deviceReady ? impl_->device.playback.internalPeriodSizeInFrames : 0;
}

AudioDevice::LoadStats AudioDevice::takeLoadStats() {
    LoadStats s;
    s.maxLoad = impl_->maxLoad.exchange(0.0f, std::memory_order_relaxed);
    const std::uint64_t busy = impl_->busyNs.exchange(0, std::memory_order_relaxed);
    const std::uint64_t frames = impl_->frames.exchange(0, std::memory_order_relaxed);
    const double rate = impl_->deviceReady ? impl_->device.sampleRate : 48000.0;
    s.averageLoad = frames ? static_cast<float>((busy * 1e-9) / (frames / rate)) : 0.0f;
    s.frames = frames;
    return s;
}

std::uint64_t AudioDevice::callbacks() const {
    return impl_->callbacks.load(std::memory_order_relaxed);
}

void AudioDevice::listDevices() {
    ma_context context;
    if (ma_context_init(kBackends, kBackendCount, nullptr, &context) != MA_SUCCESS) {
        log::error("cannot initialise the audio backend");
        return;
    }
    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
        std::printf("Audio outputs (%s):\n", ma_get_backend_name(context.backend));
        for (ma_uint32 i = 0; i < count; ++i)
            std::printf("  %s%s\n", playback[i].name, playback[i].isDefault ? "  [default]" : "");
    }
    ma_context_uninit(&context);
}

} // namespace mbx
