#include "config/EngineConfig.h"

#include "config/JsonUtil.h"

#include <sstream>

namespace mbx {

using nlohmann::json;

namespace {

// Reads obj[key] as a number in [lo, hi]; on a bad value warns and leaves `out` alone.
template <typename T>
void readNumber(const json& obj, const char* key, double lo, double hi, T& out, const std::string& where,
                std::vector<std::string>& warnings) {
    if (!obj.contains(key)) return;
    const json& v = obj[key];
    const bool integral = std::is_integral_v<T>;
    if (!v.is_number() || (integral && !v.is_number_integer()) || v.get<double>() < lo || v.get<double>() > hi) {
        std::ostringstream msg;
        msg << where << ": \"" << key << "\" must be " << (integral ? "an integer" : "a number") << " between " << lo
            << " and " << hi << " - default " << out << " used";
        warnings.push_back(msg.str());
        return;
    }
    out = static_cast<T>(v.get<double>());
}

} // namespace

bool loadEngineSettings(const std::filesystem::path& file, EngineSettings& out, bool& found,
                        std::vector<std::string>& warnings, std::string& error) {
    std::error_code ec;
    found = std::filesystem::exists(file, ec);
    if (!found) return true;

    json root;
    if (!jsonutil::readFile(file, root, error)) return false;
    if (!root.is_object()) {
        error = file.string() + ": not a JSON object";
        return false;
    }
    const std::string where = "engine.json";
    jsonutil::checkKeys(root,
                        {"version", "audio", "master_volume_db", "polyphony", "max_layers", "ram_budget_percent",
                         "midi_channel", "pitch_bend_range", "midi_serial", "control_socket", "panel", "record_folder", "dac_mute"},  // "panel": read by mbx-control
                        where, warnings);
    if (!root.contains("version") || root["version"] != 1) warnings.push_back(where + ": \"version\" should be 1");

    if (root.contains("audio")) {
        const json& audio = root["audio"];
        if (!audio.is_object()) {
            warnings.push_back(where + ": \"audio\" must be an object - defaults used");
        } else {
            jsonutil::checkKeys(audio, {"device", "sample_rate", "period_frames", "periods"}, where + " audio",
                                warnings);
            if (audio.contains("device")) {
                if (audio["device"].is_string())
                    out.audioDevice = audio["device"].get<std::string>();
                else
                    warnings.push_back(where + ": audio \"device\" must be a string - default used");
            }
            readNumber(audio, "sample_rate", 22050, 192000, out.sampleRate, where + " audio", warnings);
            readNumber(audio, "period_frames", 16, 4096, out.periodFrames, where + " audio", warnings);
            readNumber(audio, "periods", 2, 8, out.periods, where + " audio", warnings);
        }
    }
    readNumber(root, "master_volume_db", -90, 6, out.masterVolumeDb, where, warnings);
    readNumber(root, "polyphony", kMinPolyphony, kMaxPolyphony, out.polyphony, where, warnings);
    readNumber(root, "max_layers", 1, kMaxLayersLimit, out.maxLayers, where, warnings);
    readNumber(root, "ram_budget_percent", 0, 95, out.ramBudgetPercent, where, warnings);
    readNumber(root, "pitch_bend_range", 0, 24, out.pitchBendRange, where, warnings);

    if (root.contains("midi_channel")) {
        const json& c = root["midi_channel"];
        if (c.is_string() && c.get<std::string>() == "omni")
            out.midiChannel = 0;
        else if (c.is_number_integer() && c.get<int>() >= 1 && c.get<int>() <= 16)
            out.midiChannel = c.get<int>();
        else
            warnings.push_back(where + ": \"midi_channel\" must be 1..16 or \"omni\" - omni used");
    }
    if (root.contains("midi_serial")) {
        if (root["midi_serial"].is_string())
            out.midiSerial = root["midi_serial"].get<std::string>();
        else
            warnings.push_back(where + ": \"midi_serial\" must be a string (\"\" = no DIN MIDI) - default " +
                               out.midiSerial + " used");
    }
    if (root.contains("record_folder")) {
        if (root["record_folder"].is_string() && !root["record_folder"].get<std::string>().empty())
            out.recordFolder = root["record_folder"].get<std::string>();
        else
            warnings.push_back(where + ": \"record_folder\" must be a folder - /tmp used");
    }
    if (root.contains("dac_mute")) {
        const json& d = root["dac_mute"];
        if (!d.is_object()) {
            warnings.push_back(where + ": \"dac_mute\" must be an object - defaults used");
        } else {
            jsonutil::checkKeys(d, {"enabled", "a", "b", "chip"}, where + " dac_mute", warnings);
            if (d.contains("enabled")) {
                if (d["enabled"].is_boolean())
                    out.dacMute.enabled = d["enabled"].get<bool>();
                else
                    warnings.push_back(where + ": dac_mute \"enabled\" must be true or false - true used");
            }
            readNumber(d, "a", 0, 53, out.dacMute.a, where + " dac_mute", warnings);
            readNumber(d, "b", 0, 53, out.dacMute.b, where + " dac_mute", warnings);
            if (out.dacMute.a == out.dacMute.b) {
                warnings.push_back(where + ": dac_mute \"a\" and \"b\" must be different GPIOs - 16 and 17 used");
                out.dacMute.a = 16;
                out.dacMute.b = 17;
            }
            if (d.contains("chip")) {
                if (d["chip"].is_string())
                    out.dacMute.chip = d["chip"].get<std::string>();
                else
                    warnings.push_back(where + ": dac_mute \"chip\" must be a string - automatic used");
            }
        }
    }
    if (root.contains("control_socket")) {
        if (root["control_socket"].is_string())
            out.controlSocket = root["control_socket"].get<std::string>();
        else
            warnings.push_back(where + ": \"control_socket\" must be a string (\"\" = none) - default used");
    }
    return true;
}

} // namespace mbx
