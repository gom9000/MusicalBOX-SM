#include "sampler/Program.h"

#include <algorithm>
#include <cmath>

namespace mbx {

float dbToGain(float db) {
    return std::pow(10.0f, db / 20.0f);
}

const char* outputName(Output output) {
    switch (output) {
    case Output::A: return "A";
    case Output::B: return "B";
    case Output::AB: return "AB";
    }
    return "?";
}

VelocitySettings::Mapped VelocitySettings::map(int velocity) const {
    if (!enabled) return {127, 1.0f};

    const int steps = std::clamp(levels, 1, 127);
    const int v = std::clamp(velocity, 1, 127);
    // Step 1..steps; velocity 127 always lands on the top step.
    const int step = static_cast<int>(std::ceil(v * steps / 127.0));
    const float x = static_cast<float>(step) / static_cast<float>(steps);

    // The zone is chosen with the highest velocity of the step, so a step never
    // reaches into the sample layer above it (2 steps: 1..63 -> 63, 64..127 -> 127).
    const int lookup = std::clamp(step * 127 / steps, 1, 127);
    const float gain = (curve == VelocityCurve::Linear) ? x : std::pow(10.0f, -(1.0f - x) * 2.0f);
    return {lookup, gain};
}

} // namespace mbx
