// Single translation unit holding the implementations of the header-only
// third-party libraries. Keeping them here means the rest of the engine
// includes only the declarations, and these large files compile once.

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio/miniaudio.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_libs/dr_wav.h"
