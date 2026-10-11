// fx_render: runs a WAV file through an effect chain written as in
// programs.json, to compare effects offline.
//   fx_render in.wav out.wav '[{"type":"leslie","params":{"speed":"slow"}}]'

#include "fx/FxChain.h"
#include "sampler/Program.h"

#include "dr_libs/dr_wav.h"
#include "nlohmann/json.hpp"

#include <cstdio>
#include <vector>

using namespace mbx;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: fx_render in.wav out.wav FX_JSON_ARRAY\n");
        return 2;
    }
    unsigned channels = 0, rate = 0;
    drwav_uint64 frames = 0;
    float* data = drwav_open_file_and_read_pcm_frames_f32(argv[1], &channels, &rate, &frames, nullptr);
    if (!data) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    std::vector<float> L(frames), R(frames);
    for (drwav_uint64 i = 0; i < frames; ++i) {
        L[i] = data[i * channels];
        R[i] = data[i * channels + (channels > 1 ? 1 : 0)];
    }
    drwav_free(data, nullptr);

    std::vector<FxDef> defs;
    for (const auto& e : nlohmann::json::parse(argv[3])) {
        FxDef d;
        d.type = e.at("type").get<std::string>();
        d.paramsJson = e.value("params", nlohmann::json::object()).dump();
        defs.push_back(d);
    }
    std::vector<std::string> warnings;
    auto chain = FxChain::build(defs, "fx_render", warnings);
    for (auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    chain->prepare(rate);
    chain->reset();
    for (drwav_uint64 pos = 0; pos < frames; pos += 128) {
        const int n = static_cast<int>(std::min<drwav_uint64>(128, frames - pos));
        chain->process(L.data() + pos, R.data() + pos, n);
    }
    std::vector<float> out(frames * 2);
    for (drwav_uint64 i = 0; i < frames; ++i) out[2 * i] = L[i], out[2 * i + 1] = R[i];
    drwav_data_format fmt{drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, 2, rate, 32};
    drwav wav;
    if (!drwav_init_file_write(&wav, argv[2], &fmt, nullptr)) return 1;
    drwav_write_pcm_frames(&wav, frames, out.data());
    drwav_uninit(&wav);
    return 0;
}
