#pragma once

#include "fx/Effect.h"

#include <memory>
#include <string>
#include <vector>

namespace mbx {

struct FxDef;

// The effects of one program, in the order written in programs.json.
// Same threading rules as Effect: build and prepare before the audio starts,
// reset, setSwitch and process in the audio thread.
class FxChain {
public:
    // Effects with "bypass": true and unknown types are left out (with a warning).
    static std::shared_ptr<FxChain> build(const std::vector<FxDef>& defs, const std::string& where,
                                          std::vector<std::string>& warnings);

    void prepare(double sampleRate);
    void reset();
    void process(float* left, float* right, int frames);

    // The program's switch button: every effect with a "switch" goes to its
    // first (A) or second (B) value. immediate = no transition.
    void setSwitch(bool positionB, bool immediate);
    bool hasSwitch() const { return hasSwitch_; }

    std::size_t size() const { return effects_.size(); }
    bool prepared() const { return prepared_; }

private:
    std::vector<std::unique_ptr<Effect>> effects_;
    bool hasSwitch_ = false;
    bool prepared_ = false;
};

} // namespace mbx
