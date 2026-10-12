#include "fx/Effects.h"

#include <algorithm>

namespace mbx {

Motor::Motor(FxParams& p)
    : stopS_(p.number("stop_s", 3.0f, 0.1f, 30.0f)), startS_(p.number("start_s", 1.0f, 0.1f, 30.0f)) {
    const std::string state = p.choice("state", "on", {"on", "off"});
    onA_ = state == "on";
    running_ = onA_;
    speed_ = running_ ? 1.0f : 0.0f;
}

void Motor::prepare(double sampleRate) {
    down_ = static_cast<float>(1.0 / (stopS_ * sampleRate));
    up_ = static_cast<float>(1.0 / (startS_ * sampleRate));
    reset();
}

void Motor::reset() {
    running_ = onA_;
    speed_ = running_ ? 1.0f : 0.0f;
}

bool Motor::configureSwitch(const std::string& param, const nlohmann::json& a, const nlohmann::json& b) {
    auto state = [](const nlohmann::json& v, bool& on) {
        if (!v.is_string() || (v != "on" && v != "off")) return false;
        on = v == "on";
        return true;
    };
    return param == "state" && state(a, onA_) && state(b, onB_);
}

void Motor::setSwitch(bool positionB, bool immediate) {
    running_ = positionB ? onB_ : onA_;
    if (immediate) speed_ = running_ ? 1.0f : 0.0f;
}

float Motor::advance(int frames) {
    speed_ = running_ ? std::min(1.0f, speed_ + up_ * frames) : std::max(0.0f, speed_ - down_ * frames);
    return speed_;
}

} // namespace mbx
