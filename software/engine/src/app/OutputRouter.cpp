#include "app/OutputRouter.h"

#include "core/Engine.h"

#include <algorithm>

namespace mbx {

OutputRouter::OutputRouter(const Engine& engine, SetPins setPins) : engine_(engine), setPins_(std::move(setPins)) {
    if (setPins_) setPins_(false, false);
}

OutputRouter::~OutputRouter() { stop(); }

int OutputRouter::pinsFor(int output) {
    switch (output) {
    case 0: return 1;  // A
    case 1: return 2;  // B
    case 2: return 3;  // AB
    default: return 0;
    }
}

void OutputRouter::begin(double delayMs, Clock::time_point now) {
    delay_ = std::chrono::microseconds(static_cast<long long>(std::max(0.0, delayMs) * 1000.0));
    earliest_ = now + std::chrono::milliseconds(kStartupMs);
    pending_ = applied_;
    dueAt_ = now;
}

void OutputRouter::poll(Clock::time_point now) {
    const int wanted = pinsFor(engine_.currentOutput());
    if (wanted != pending_) {
        pending_ = wanted;
        dueAt_ = now + delay_;
    }
    if (pending_ != applied_ && now >= dueAt_ && now >= earliest_) {
        applied_ = pending_;
        if (setPins_) setPins_(applied_ & 1, applied_ & 2);
    }
}

void OutputRouter::start(double delayMs) {
    if (running_.exchange(true)) return;
    begin(delayMs, Clock::now());
    thread_ = std::thread([this] {
        while (running_.load(std::memory_order_relaxed)) {
            poll(Clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
    });
}

void OutputRouter::stop() {
    if (running_.exchange(false) && thread_.joinable()) thread_.join();
    if (setPins_) setPins_(false, false);
    applied_ = pending_ = 0;
}

} // namespace mbx
