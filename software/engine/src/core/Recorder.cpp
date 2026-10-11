#include "core/Recorder.h"

#include "core/Log.h"

#include "dr_libs/dr_wav.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace mbx {

namespace {
constexpr double kBufferSeconds = 4.0;
constexpr std::uintmax_t kMinFreeBytes = 8u << 20;  // stop before the disk (or /tmp in RAM) is full
}

Recorder::~Recorder() {
    stop();
}

bool Recorder::start(const std::string& path, unsigned sampleRate, std::string& error) {
    stop();
    std::size_t size = 1;
    while (size < static_cast<std::size_t>(kBufferSeconds * sampleRate * 2)) size <<= 1;
    ring_.assign(size, 0.0f);
    mask_ = size - 1;
    head_ = tail_ = 0;
    written_ = dropped_ = 0;
    stop_ = failed_ = false;
    sampleRate_ = sampleRate;

    auto* wav = new drwav;
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = sampleRate;
    fmt.bitsPerSample = 16;
    if (!drwav_init_file_write(wav, path.c_str(), &fmt, nullptr)) {
        delete wav;
        error = "cannot write " + path;
        return false;
    }
    wav_ = wav;
    path_ = path;
    thread_ = std::thread(&Recorder::run, this);
    return true;
}

void Recorder::stop() {
    if (!wav_) return;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    auto* wav = static_cast<drwav*>(wav_);
    drwav_uninit(wav);  // writes the final sizes in the header
    delete wav;
    wav_ = nullptr;
}

void Recorder::push(const float* stereo, int frames) {
    const std::size_t n = static_cast<std::size_t>(frames) * 2;
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (failed_.load(std::memory_order_relaxed) || ring_.size() - (head - tail) < n) {
        dropped_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
        return;
    }
    for (std::size_t i = 0; i < n; ++i) ring_[(head + i) & mask_] = stereo[i];
    head_.store(head + n, std::memory_order_release);
}

void Recorder::run() {
    auto* wav = static_cast<drwav*>(wav_);
    std::vector<std::int16_t> block;
    std::uint32_t rng = 22222;  // TPDF dither: two uniform noises of one LSB
    auto noise = [&] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    };
    auto lastCheck = std::chrono::steady_clock::now();
    for (;;) {
        const bool stopping = stop_.load();
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t n = head - tail;
        if (n > 0 && !failed_) {
            block.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                const float x = ring_[(tail + i) & mask_] * 32767.0f + (noise() - noise());
                block[i] = static_cast<std::int16_t>(std::lround(std::clamp(x, -32768.0f, 32767.0f)));
            }
            const drwav_uint64 frames = n / 2;
            if (drwav_write_pcm_frames(wav, frames, block.data()) != frames) {
                failed_ = true;
                log::warn("recording %s: write error - stopped, %.0f s kept", path_.c_str(), seconds());
            } else {
                written_.fetch_add(frames);
            }
        }
        tail_.store(head, std::memory_order_release);
        if (stopping) break;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastCheck > std::chrono::seconds(1) && !failed_) {
            lastCheck = now;
            std::error_code ec;
            const auto space = std::filesystem::space(std::filesystem::path(path_).parent_path(), ec);
            if (!ec && space.available < kMinFreeBytes) {
                failed_ = true;
                log::warn("recording %s: disk almost full - stopped, %.0f s kept", path_.c_str(), seconds());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace mbx
