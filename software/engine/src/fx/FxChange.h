#pragma once

namespace mbx {

// A change to apply to an effect chain that keeps playing: a programmed value,
// the values of a switch, or a control moved by hand (see FxChain).
struct FxChange {
    static constexpr int kBypass = -1;        // param: bypass (value >= 0.5 = bypassed)
    static constexpr int kSwitchValues = -2;  // param: new switch values (value = A, value2 = B)
    int slot = 0;                             // effect, in chain order (0 = first)
    int param = 0;                            // parameter index of the effect type
    float value = 0.0f, value2 = 0.0f;
};

} // namespace mbx
