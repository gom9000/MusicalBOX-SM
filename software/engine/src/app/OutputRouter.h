#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace mbx {

class Engine;

// Program "output" A / B / AB -> mute pins of the two DACs (XSMT of the
// PCM5102A: high = playing, low = muted, with the DAC's own soft ramp).
// Both DACs receive the same I2S stream; the program chooses which one sounds.
//
// The pins follow the engine with the delay of the audio buffer, so that they
// change when the new program's sound reaches the DACs, not when it is
// rendered. Nothing is unmuted before start() + kStartupMs (the DACs settle
// on the clock without a pop), and stop() mutes both.
class OutputRouter {
public:
    using Clock = std::chrono::steady_clock;
    using SetPins = std::function<void(bool a, bool b)>;  // true = playing
    static constexpr int kStartupMs = 150;
    static constexpr int kPollMs = 2;

    OutputRouter(const Engine& engine, SetPins setPins);
    ~OutputRouter();
    OutputRouter(const OutputRouter&) = delete;
    OutputRouter& operator=(const OutputRouter&) = delete;

    // Thread that follows the engine every kPollMs. delayMs = audio buffer.
    void start(double delayMs);
    void stop();  // mutes both, waits for the thread

    // One step at time `now` (the thread calls it; tests call it directly).
    void begin(double delayMs, Clock::time_point now);
    void poll(Clock::time_point now);
    bool pinA() const { return applied_ & 1; }
    bool pinB() const { return applied_ & 2; }

    static int pinsFor(int output);  // engine output (-1, 0 A, 1 B, 2 AB) -> bit 0 A, bit 1 B

private:
    const Engine& engine_;
    SetPins setPins_;
    Clock::duration delay_{};
    Clock::time_point earliest_{};
    Clock::time_point dueAt_{};
    int pending_ = 0;
    int applied_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace mbx
