#include "sampler/WavLoader.h"

#include "dr_libs/dr_wav.h"

#include <cstdlib>

namespace mbx {

bool loadWav(const std::string& path, Zone& zone, std::string& error) {
    drwav wav;
    if (!drwav_init_file_with_metadata(&wav, path.c_str(), 0, nullptr)) {
        error = "cannot open or parse WAV";
        return false;
    }

    if (wav.channels < 1 || wav.channels > 2) {
        error = "unsupported channel count " + std::to_string(wav.channels) + " (mono or stereo only)";
        drwav_uninit(&wav);
        return false;
    }
    if (wav.totalPCMFrameCount < 2) {
        error = "file is empty";
        drwav_uninit(&wav);
        return false;
    }

    zone.file = path;
    zone.channels = wav.channels;
    zone.sampleRate = wav.sampleRate;
    zone.frames = static_cast<std::uint32_t>(wav.totalPCMFrameCount);
    zone.data.assign(static_cast<std::size_t>(zone.frames) * zone.channels, 0);

    const drwav_uint64 read = drwav_read_pcm_frames_s16(&wav, zone.frames, zone.data.data());
    if (read < zone.frames) {
        // Truncated file: keep what was read.
        zone.frames = static_cast<std::uint32_t>(read);
        zone.data.resize(static_cast<std::size_t>(zone.frames) * zone.channels);
    }

    // A "stereo" file whose two channels are the same (many libraries of mono
    // instruments are saved this way) is kept as mono: half the memory, same sound.
    if (zone.channels == 2) {
        constexpr int kTolerance = 2;  // in 16-bit steps: rounding and dither
        bool same = true;
        for (std::size_t i = 0; same && i < zone.data.size(); i += 2)
            same = std::abs(zone.data[i] - zone.data[i + 1]) <= kTolerance;
        if (same) {
            for (std::size_t f = 0; f < zone.frames; ++f)
                zone.data[f] = static_cast<std::int16_t>((zone.data[2 * f] + zone.data[2 * f + 1]) / 2);
            zone.data.resize(zone.frames);
            zone.data.shrink_to_fit();
            zone.channels = 1;
            zone.dualMono = true;
        }
    }

    zone.hasLoop = false;
    for (drwav_uint32 i = 0; i < wav.metadataCount; ++i) {
        const drwav_metadata& meta = wav.pMetadata[i];
        if (meta.type != drwav_metadata_type_smpl || meta.data.smpl.sampleLoopCount == 0) continue;
        const drwav_smpl_loop& loop = meta.data.smpl.pLoops[0];
        if (loop.firstSampleOffset < loop.lastSampleOffset && loop.lastSampleOffset < zone.frames) {
            zone.hasLoop = true;
            zone.loopStart = loop.firstSampleOffset;
            zone.loopEnd = loop.lastSampleOffset;
        }
        break;
    }

    drwav_uninit(&wav);
    return zone.frames >= 2;
}

} // namespace mbx
