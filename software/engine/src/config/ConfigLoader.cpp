#include "config/ConfigLoader.h"

#include <algorithm>

#include "config/JsonUtil.h"

#include <set>

namespace mbx {

using nlohmann::json;

namespace {

bool parseInstrument(const json& j, InstrumentDef& def, std::string& problem, std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        problem = "entry is not an object";
        return false;
    }
    const std::string where = (j.contains("id") && j["id"].is_string()) ? j["id"].get<std::string>() : "?";
    jsonutil::checkKeys(j, {"id", "name", "path", "files", "velocity_layers", "fill", "name_octave"}, "instrument " + where,
                        warnings);
    for (const char* key : {"id", "path", "files"}) {
        if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
            problem = std::string("missing or empty \"") + key + "\"";
            return false;
        }
    }
    def.id = j["id"].get<std::string>();
    def.path = j["path"].get<std::string>();
    def.files = j["files"].get<std::string>();
    def.name = j.value("name", def.id);

    bool layersDone = false;
    if (j.contains("velocity_layers")) {
        const json& layers = j["velocity_layers"];
        if (layers.is_object() && !layers.empty()) {
            // { "PP": 1, "MP": 50, "F": 90, "FF": 115 }: label in the file name -> lowest velocity.
            for (auto it = layers.begin(); it != layers.end(); ++it) {
                if (!it.value().is_number_integer() || it.value().get<int>() < 1 || it.value().get<int>() > 127 ||
                    it.key().empty()) {
                    problem = "\"velocity_layers\": \"" + it.key() + "\" must be a velocity in 1..127";
                    return false;
                }
                def.velocityLabels.emplace_back(it.key(), it.value().get<int>());
            }
            std::sort(def.velocityLabels.begin(), def.velocityLabels.end(),
                      [](const auto& a, const auto& b) { return a.second < b.second; });
            for (std::size_t i = 0; i < def.velocityLabels.size(); ++i) {
                if (i > 0 && def.velocityLabels[i].second == def.velocityLabels[i - 1].second) {
                    problem = "\"velocity_layers\": two labels with the same velocity";
                    return false;
                }
                def.velocityLayers.push_back(def.velocityLabels[i].second);
            }
            layersDone = true;
        } else if (!layers.is_array() || layers.empty()) {
            problem = "\"velocity_layers\" must be a non-empty array of velocities or an object of labels";
            return false;
        }
        int previous = 0;
        for (const json& v : layersDone ? json::array() : layers) {
            if (!v.is_number_integer()) {
                problem = "\"velocity_layers\" must contain integers";
                return false;
            }
            const int value = v.get<int>();
            if (value <= previous || value > 127) {
                problem = "\"velocity_layers\" must be increasing values in 1..127";
                return false;
            }
            def.velocityLayers.push_back(value);
            previous = value;
        }
    }

    // Octave convention of {name}: "C3" (C3 = 60, default) or "C4" (C4 = 60, scientific pitch).
    const std::string octave = j.value("name_octave", std::string("C3"));
    if (octave == "C3") {
        def.nameOffset = 0;
    } else if (octave == "C4") {
        def.nameOffset = -12;
    } else {
        problem = "\"name_octave\" must be \"C3\" or \"C4\"";
        return false;
    }

    const std::string fill = j.value("fill", std::string("nearest"));
    if (fill == "nearest") {
        def.fill = FillMode::Nearest;
    } else if (fill == "none") {
        def.fill = FillMode::None;
    } else {
        problem = "\"fill\" must be \"nearest\" or \"none\"";
        return false;
    }
    return true;
}

} // namespace

bool loadInstrumentsConfig(const std::filesystem::path& file, InstrumentsConfig& out,
                           std::vector<std::string>& warnings, std::string& error) {
    json root;
    if (!jsonutil::readFile(file, root, error)) return false;

    if (!root.is_object() || !root.contains("instruments") || !root["instruments"].is_array()) {
        error = file.string() + ": missing \"instruments\" array";
        return false;
    }
    jsonutil::checkKeys(root, {"version", "media_root", "instruments"}, file.filename().string(), warnings);
    if (!root.contains("version") || root["version"] != 1) {
        warnings.push_back(file.string() + ": \"version\" should be 1");
    }

    std::filesystem::path media = "../media";
    if (root.contains("media_root")) {
        if (!root["media_root"].is_string()) {
            error = file.string() + ": \"media_root\" must be a string";
            return false;
        }
        media = root["media_root"].get<std::string>();
    }
    if (media.is_relative()) media = file.parent_path() / media;
    out.mediaRoot = media.lexically_normal();

    std::set<std::string> seen;
    int index = 0;
    for (const json& entry : root["instruments"]) {
        ++index;
        InstrumentDef def;
        std::string problem;
        bool ok = false;
        try {
            ok = parseInstrument(entry, def, problem, warnings);
        } catch (const json::exception& e) {
            problem = e.what();
        }
        if (!ok) {
            warnings.push_back("instrument #" + std::to_string(index) + ": " + problem + " - skipped");
            continue;
        }
        if (!seen.insert(def.id).second) {
            warnings.push_back("instrument " + def.id + ": duplicate id - skipped");
            continue;
        }
        out.instruments.push_back(std::move(def));
    }
    return true;
}

} // namespace mbx
