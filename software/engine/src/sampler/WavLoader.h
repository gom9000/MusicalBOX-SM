#pragma once

#include "sampler/Zone.h"

#include <string>

namespace mbx {

// Loads a WAV file (8/16/24/32-bit integer or float, mono or stereo) into a
// Zone as 16-bit audio, and reads the first loop of the 'smpl' chunk if any.
// Root note and velocity range are left to the caller.
bool loadWav(const std::string& path, Zone& zone, std::string& error);

} // namespace mbx
