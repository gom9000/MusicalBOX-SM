#include "fx/Effects.h"

#include <algorithm>

namespace mbx {

void Motor::update(bool immediate) {
    down_ = static_cast<float>(1.0 / (param(kStop) * sampleRate_));
    up_ = static_cast<float>(1.0 / (param(kStart) * sampleRate_));
    running_ = choice(kState) == 0;
    if (immediate) speed_ = running_ ? 1.0f : 0.0f;
}

void Motor::reset() { speed_ = running_ ? 1.0f : 0.0f; }

float Motor::advance(int frames) {
    speed_ = running_ ? std::min(1.0f, speed_ + up_ * frames) : std::max(0.0f, speed_ - down_ * frames);
    return speed_;
}

} // namespace mbx
