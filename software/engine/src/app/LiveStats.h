#pragma once

#include <mutex>

namespace mbx {

// Latest audio figures, written by the main loop about once a second and
// read by the control socket (status).
class LiveStats {
public:
    struct Snapshot {
        float loadAverage = 0.0f;   // share of the audio period used, 0..1
        float loadMax = 0.0f;
        float peak = 0.0f;          // 1.0 = 0 dBFS, before the limiter
        float limiterGain = 1.0f;   // lowest limiter gain, 1.0 = not active
        int maxVoices = 0;
        double uptimeSeconds = 0.0;
    };
    void set(const Snapshot& s) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = s;
    }
    Snapshot get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    Snapshot snapshot_;
};

} // namespace mbx
