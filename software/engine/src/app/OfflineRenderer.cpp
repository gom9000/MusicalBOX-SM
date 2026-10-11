#include "app/OfflineRenderer.h"

#include "core/Engine.h"

#include "dr_libs/dr_wav.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace mbx {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, sep)) parts.push_back(part);
    return parts;
}

} // namespace

bool parseEventList(const std::string& text, std::vector<ScheduledEvent>& out, std::string& error) {
    for (const std::string& item : split(text, ',')) {
        if (item.empty()) continue;
        const std::string usage = "\"" + item + "\": expected note@start:duration[:velocity], pSLOT@start or w@start";
        const auto at = item.find('@');
        if (at == std::string::npos) {
            error = usage;
            return false;
        }
        ScheduledEvent e;
        try {
            std::size_t used = 0;
            if ((item[0] == 'w' || item[0] == 'W') && at == 1) {
                e.kind = ScheduledEvent::Kind::Switch;
                e.start = std::stod(item.substr(at + 1), &used);
                if (used != item.size() - at - 1) throw std::invalid_argument("time");
            } else if (item[0] == 'p' || item[0] == 'P') {
                e.kind = ScheduledEvent::Kind::Preset;
                e.slot = std::stoi(item.substr(1, at - 1), &used);
                if (used != at - 1) throw std::invalid_argument("slot");
                e.start = std::stod(item.substr(at + 1), &used);
                if (used != item.size() - at - 1) throw std::invalid_argument("time");
            } else {
                e.note = std::stoi(item.substr(0, at), &used);
                if (used != at) throw std::invalid_argument("note");
                const auto fields = split(item.substr(at + 1), ':');
                if (fields.size() < 2 || fields.size() > 3) throw std::invalid_argument("fields");
                e.start = std::stod(fields[0]);
                e.duration = std::stod(fields[1]);
                if (fields.size() == 3) e.velocity = std::stoi(fields[2]);
            }
        } catch (const std::exception&) {
            error = usage;
            return false;
        }
        const bool bad = e.start < 0 ||
                         (e.kind == ScheduledEvent::Kind::Note &&
                          (e.note < 0 || e.note > 127 || e.velocity < 1 || e.velocity > 127 || e.duration <= 0)) ||
                         (e.kind == ScheduledEvent::Kind::Preset && (e.slot < 1 || e.slot > kPresetCount));
        if (bad) {
            error = "\"" + item + "\": value out of range";
            return false;
        }
        out.push_back(e);
    }
    if (out.empty()) {
        error = "no events given";
        return false;
    }
    return true;
}

bool renderOffline(Engine& engine, const std::vector<ScheduledEvent>& events, double tail,
                   const std::string& wavPath, std::string& error) {
    struct Timed {
        std::int64_t frame;
        EngineEvent event;
    };
    const double rate = engine.sampleRate();
    auto frameAt = [&](double seconds) { return static_cast<std::int64_t>(std::llround(seconds * rate)); };

    std::vector<Timed> timed;
    double end = 0.0;
    for (const auto& e : events) {
        if (e.kind == ScheduledEvent::Kind::Switch) {
            timed.push_back({frameAt(e.start), EngineEvent::fxSwitch()});
            end = std::max(end, e.start);
            continue;
        }
        if (e.kind == ScheduledEvent::Kind::Preset) {
            timed.push_back({frameAt(e.start), EngineEvent::selectPreset(e.slot)});
            end = std::max(end, e.start);
            continue;
        }
        const auto note = static_cast<std::uint8_t>(e.note);
        timed.push_back({frameAt(e.start),
                         EngineEvent::fromMidi({midi::kNoteOn, note, static_cast<std::uint8_t>(e.velocity)})});
        timed.push_back({frameAt(e.start + e.duration), EngineEvent::fromMidi({midi::kNoteOff, note, 0})});
        end = std::max(end, e.start + e.duration);
    }
    std::stable_sort(timed.begin(), timed.end(), [](const Timed& a, const Timed& b) { return a.frame < b.frame; });

    const int source = engine.registerSource();
    const auto total = static_cast<std::int64_t>(std::ceil((end + tail) * rate));
    constexpr int kBlock = 64;  // small blocks keep event timing within ~1.3 ms
    std::vector<float> audio(static_cast<std::size_t>(total) * 2, 0.0f);

    std::size_t next = 0;
    for (std::int64_t pos = 0; pos < total; pos += kBlock) {
        while (next < timed.size() && timed[next].frame <= pos) {
            engine.post(source, timed[next].event);
            ++next;
        }
        const int n = static_cast<int>(std::min<std::int64_t>(kBlock, total - pos));
        engine.render(audio.data() + 2 * pos, n);
    }
    engine.releaseSource(source);

    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = 2;
    format.sampleRate = static_cast<drwav_uint32>(rate);
    format.bitsPerSample = 32;
    drwav wav;
    if (!drwav_init_file_write(&wav, wavPath.c_str(), &format, nullptr)) {
        error = "cannot write " + wavPath;
        return false;
    }
    drwav_write_pcm_frames(&wav, static_cast<drwav_uint64>(total), audio.data());
    drwav_uninit(&wav);
    return true;
}

} // namespace mbx
