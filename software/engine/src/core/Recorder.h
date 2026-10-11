#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace mbx {

// Writes what the engine plays to a WAV file (stereo, 16 bit), for listening
// back or comparing. The audio thread only copies samples into a ring buffer
// (no allocation, no lock, no disk); a writer thread empties it to the file.
// If the writer cannot keep up, samples are dropped and counted, never waited for.
class Recorder {
public:
    ~Recorder();

    // Opens the file and starts the writer. False (error set) on failure.
    bool start(const std::string& path, unsigned sampleRate, std::string& error);
    // Stops the writer, writes the rest and closes the file. Call only when
    // the audio thread no longer pushes (see Engine::setRecorder).
    void stop();

    // Audio thread: interleaved stereo frames.
    void push(const float* stereo, int frames);

    const std::string& path() const { return path_; }
    double seconds() const { return static_cast<double>(written_.load()) / sampleRate_; }
    std::uint64_t droppedFrames() const { return dropped_.load(); }
    // True when the writer gave up (disk full, write error): stop and keep what was written.
    bool failed() const { return failed_.load(); }

private:
    void run();

    std::string path_;
    unsigned sampleRate_ = 48000;
    std::vector<float> ring_;  // interleaved stereo, power-of-two size
    std::size_t mask_ = 0;
    std::atomic<std::size_t> head_{0}, tail_{0};  // in floats
    std::atomic<std::uint64_t> written_{0}, dropped_{0};
    std::atomic<bool> stop_{false}, failed_{false};
    std::thread thread_;
    void* wav_ = nullptr;  // drwav*
};

} // namespace mbx
