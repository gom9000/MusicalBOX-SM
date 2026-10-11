#pragma once

// Fixed limits of the engine. The values a user may want to change
// (polyphony, layers per program, volume, audio settings...) live in
// config/engine.json; here are only their defaults and allowed ranges.

namespace mbx {

// Notes sounding at the same time ("polyphony" in engine.json).
constexpr int kDefaultPolyphony = 64;
constexpr int kMinPolyphony = 8;
constexpr int kMaxPolyphony = 256;

// Instruments per program, stacked or split ("max_layers" in engine.json).
constexpr int kDefaultMaxLayers = 3;
constexpr int kMaxLayersLimit = 8;

// Extra voice slots used only while a stolen voice fades out (see VoicePool).
constexpr int kStealReserve = 8;

// The engine renders in chunks of at most this many frames, whatever block
// size the audio device asks for. Scratch buffers are sized on this.
constexpr int kMaxBlockFrames = 512;

// Fade applied when a voice is cut (stolen, program change, all sound off).
constexpr float kKillFadeMs = 5.0f;

// Time constant of the master-volume smoothing.
constexpr float kGainSmoothingMs = 5.0f;

// On a program change the effects of the old program (echo tails...) fade out in this time.
constexpr float kFxFadeMs = 30.0f;

// Master limiter: when many voices add up above this level, the volume is
// lowered smoothly instead of clipping (see Limiter).
constexpr float kLimiterThresholdDb = -1.0f;
constexpr float kLimiterReleaseMs = 120.0f;

} // namespace mbx
