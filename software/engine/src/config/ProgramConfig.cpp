#include "config/ProgramConfig.h"

#include "config/JsonUtil.h"
#include "core/Constants.h"
#include "fx/FxChain.h"
#include "sampler/InstrumentStore.h"
#include "sampler/NoteName.h"

#include <set>
#include <sstream>

namespace mbx {

using nlohmann::json;

namespace {

// Thrown by the field readers; caught per layer / per program.
struct Bad {
    std::string message;
};

double number(const json& obj, const char* key, double lo, double hi) {
    const json& v = obj.at(key);
    if (!v.is_number()) throw Bad{std::string("\"") + key + "\" must be a number"};
    const double d = v.get<double>();
    if (d < lo || d > hi) {
        std::ostringstream msg;
        msg << '"' << key << "\" must be between " << lo << " and " << hi;
        throw Bad{msg.str()};
    }
    return d;
}

int integer(const json& obj, const char* key, int lo, int hi) {
    const json& v = obj.at(key);
    if (!v.is_number_integer()) throw Bad{std::string("\"") + key + "\" must be an integer"};
    const int i = v.get<int>();
    if (i < lo || i > hi)
        throw Bad{std::string("\"") + key + "\" must be between " + std::to_string(lo) + " and " + std::to_string(hi)};
    return i;
}

std::string text(const json& obj, const char* key) {
    const json& v = obj.at(key);
    if (!v.is_string()) throw Bad{std::string("\"") + key + "\" must be a string"};
    return v.get<std::string>();
}

// A note given as MIDI number (60) or name ("C3").
int note(const json& v) {
    if (v.is_number_integer()) {
        const int n = v.get<int>();
        if (n >= 0 && n <= 127) return n;
    } else if (v.is_string()) {
        const int n = parseNoteName(v.get<std::string>());
        if (n >= 0) return n;
    }
    throw Bad{"notes must be 0..127 or a name such as \"C3\""};
}

void readAdsr(const json& j, Adsr& adsr, const std::string& where, std::vector<std::string>& warnings) {
    if (!j.is_object()) throw Bad{"\"adsr\" must be an object"};
    jsonutil::checkKeys(j, {"attack_ms", "decay_ms", "sustain", "release_ms"}, where + " adsr", warnings);
    if (j.contains("attack_ms")) adsr.attackMs = static_cast<float>(number(j, "attack_ms", 0, 10000));
    if (j.contains("decay_ms")) adsr.decayMs = static_cast<float>(number(j, "decay_ms", 0, 30000));
    if (j.contains("sustain")) adsr.sustain = static_cast<float>(number(j, "sustain", 0, 1));
    if (j.contains("release_ms")) adsr.releaseMs = static_cast<float>(number(j, "release_ms", 0, 30000));
}

void readVelocity(const json& j, VelocitySettings& v, const std::string& where, std::vector<std::string>& warnings) {
    if (!j.is_object()) throw Bad{"\"velocity\" must be an object"};
    jsonutil::checkKeys(j, {"mode", "levels", "curve"}, where + " velocity", warnings);
    if (j.contains("mode")) {
        const std::string mode = text(j, "mode");
        if (mode == "on")
            v.enabled = true;
        else if (mode == "off")
            v.enabled = false;
        else
            throw Bad{"velocity \"mode\" must be \"on\" or \"off\""};
    }
    if (j.contains("levels")) v.levels = integer(j, "levels", 1, 127);
    if (j.contains("curve")) {
        const std::string curve = text(j, "curve");
        if (curve == "linear")
            v.curve = VelocityCurve::Linear;
        else if (curve == "exp")
            v.curve = VelocityCurve::Exp;
        else
            throw Bad{"velocity \"curve\" must be \"linear\" or \"exp\""};
    }
}

bool readLoop(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) {
        if (v.get<std::string>() == "on") return true;
        if (v.get<std::string>() == "off") return false;
    }
    throw Bad{"\"loop\" must be \"on\" or \"off\""};
}

// Fields shared by program and layer: what the layer does not set, it
// inherits from the program.
struct Common {
    Adsr adsr;
    VelocitySettings velocity;
    bool loop = false;
};

void readCommon(const json& j, Common& c, const std::string& where, std::vector<std::string>& warnings) {
    if (j.contains("adsr")) readAdsr(j["adsr"], c.adsr, where, warnings);
    if (j.contains("velocity")) readVelocity(j["velocity"], c.velocity, where, warnings);
    if (j.contains("loop")) c.loop = readLoop(j["loop"]);
}

Layer readLayer(const json& j, const Common& inherited, const InstrumentStore& store, const std::string& where,
                std::vector<std::string>& warnings) {
    if (!j.is_object()) throw Bad{"layer is not an object"};
    jsonutil::checkKeys(j, {"instrument", "keys", "transpose", "gain_db", "adsr", "velocity", "loop"}, where, warnings);

    Layer layer;
    if (!j.contains("instrument")) throw Bad{"missing \"instrument\""};
    const std::string id = text(j, "instrument");
    layer.instrument = store.find(id);
    if (!layer.instrument) throw Bad{"instrument " + id + " is not loaded"};

    if (j.contains("keys")) {
        const json& k = j["keys"];
        if (!k.is_array() || k.size() != 2) throw Bad{"\"keys\" must be [low, high]"};
        layer.keyLo = note(k[0]);
        layer.keyHi = note(k[1]);
        if (layer.keyLo > layer.keyHi) throw Bad{"\"keys\": low note above high note"};
    }
    if (j.contains("transpose")) layer.transpose = integer(j, "transpose", -48, 48);
    if (j.contains("gain_db")) layer.gain = dbToGain(static_cast<float>(number(j, "gain_db", -60, 12)));

    Common c = inherited;
    readCommon(j, c, where, warnings);
    layer.adsr = c.adsr;
    layer.velocity = c.velocity;
    layer.loop = c.loop;
    return layer;
}

std::unique_ptr<Program> readProgram(const json& j, const InstrumentStore& store, int maxLayers,
                                     std::vector<std::string>& warnings) {
    if (!j.is_object()) throw Bad{"entry is not an object"};
    if (!j.contains("id")) throw Bad{"missing \"id\""};
    auto program = std::make_unique<Program>();
    program->id = text(j, "id");
    if (program->id.empty()) throw Bad{"empty \"id\""};
    const std::string where = "program " + program->id;
    jsonutil::checkKeys(j, {"id", "name", "output", "gain_db", "velocity", "loop", "adsr", "layers", "fx"}, where,
                        warnings);

    program->name = j.contains("name") ? text(j, "name") : program->id;
    if (j.contains("output")) {
        const std::string out = text(j, "output");
        if (out == "A")
            program->output = Output::A;
        else if (out == "B")
            program->output = Output::B;
        else if (out == "AB")
            program->output = Output::AB;
        else
            throw Bad{"\"output\" must be \"A\", \"B\" or \"AB\""};
    }
    if (j.contains("gain_db")) program->gain = dbToGain(static_cast<float>(number(j, "gain_db", -60, 12)));

    Common common;
    readCommon(j, common, where, warnings);

    if (!j.contains("layers") || !j["layers"].is_array()) throw Bad{"missing \"layers\" array"};
    if (j["layers"].size() > static_cast<std::size_t>(maxLayers))
        warnings.push_back(where + ": " + std::to_string(j["layers"].size()) + " layers, only the first " +
                           std::to_string(maxLayers) + " are used (max_layers in engine.json)");
    int index = 0;
    for (const json& lj : j["layers"]) {
        if (++index > maxLayers) break;
        const std::string lwhere = where + " layer " + std::to_string(index);
        try {
            program->layers.push_back(readLayer(lj, common, store, lwhere, warnings));
        } catch (const Bad& bad) {
            warnings.push_back(lwhere + ": " + bad.message + " - layer skipped");
        }
    }
    if (program->layers.empty()) throw Bad{"no usable layer"};

    if (j.contains("fx")) {
        if (!j["fx"].is_array()) throw Bad{"\"fx\" must be an array"};
        for (const json& fj : j["fx"]) {
            if (!fj.is_object() || !fj.contains("type")) throw Bad{"every effect needs a \"type\""};
            jsonutil::checkKeys(fj, {"type", "params", "bypass", "switch", "cc"}, where + " fx", warnings);
            FxDef fx;
            fx.type = text(fj, "type");
            if (fj.contains("params")) {
                if (!fj["params"].is_object()) throw Bad{"effect \"params\" must be an object"};
                fx.paramsJson = fj["params"].dump();
            }
            if (fj.contains("bypass")) {
                if (!fj["bypass"].is_boolean()) throw Bad{"effect \"bypass\" must be true or false"};
                fx.bypass = fj["bypass"].get<bool>();
            }
            if (fj.contains("switch")) {
                const json& sw = fj["switch"];
                if (!sw.is_array() || sw.size() != 3 || !sw[0].is_string())
                    throw Bad{"effect \"switch\" must be [\"parameter\", first value, second value]"};
                if (sw[1].is_object() || sw[1].is_array() || sw[2].is_object() || sw[2].is_array())
                    throw Bad{"effect \"switch\" values must be numbers, words or true/false"};
                fx.switchParam = sw[0].get<std::string>();
                fx.switchA = sw[1].dump();
                fx.switchB = sw[2].dump();
            }
            if (fj.contains("cc")) {
                if (!fj["cc"].is_object()) throw Bad{"effect \"cc\" must be an object: { \"parameter\": controller }"};
                fx.ccJson = fj["cc"].dump();
            }
            program->fx.push_back(std::move(fx));
        }
    }
    program->fxChain = FxChain::build(program->fx, where, warnings);
    return program;
}

} // namespace

const Program* ProgramBank::find(const std::string& id) const {
    for (const auto& p : programs)
        if (p->id == id) return p.get();
    return nullptr;
}

int ProgramBank::fxDefinitions() const {
    int n = 0;
    for (const auto& p : programs) n += static_cast<int>(p->fx.size());
    return n;
}

bool loadPrograms(const json& root, const InstrumentStore& store, ProgramBank& bank,
                  std::vector<std::string>& warnings, std::string& error, int maxLayers) {
    if (!root.is_object() || !root.contains("programs") || !root["programs"].is_array()) {
        error = "programs.json: missing \"programs\" array";
        return false;
    }
    jsonutil::checkKeys(root, {"version", "programs"}, "programs.json", warnings);
    if (!root.contains("version") || root["version"] != 1) warnings.push_back("programs.json: \"version\" should be 1");

    std::set<std::string> seen;
    int index = 0;
    for (const json& pj : root["programs"]) {
        ++index;
        try {
            auto program = readProgram(pj, store, maxLayers, warnings);
            if (!seen.insert(program->id).second) {
                warnings.push_back("program " + program->id + ": duplicate id - skipped");
                continue;
            }
            bank.programs.push_back(std::move(program));
        } catch (const Bad& bad) {
            std::string name = "#" + std::to_string(index);
            if (pj.is_object() && pj.contains("id") && pj["id"].is_string()) name = pj["id"].get<std::string>();
            warnings.push_back("program " + name + ": " + bad.message + " - skipped");
        } catch (const json::exception& e) {
            warnings.push_back("program #" + std::to_string(index) + ": " + e.what() + " - skipped");
        }
    }
    return true;
}

bool loadPresets(const json& root, ProgramBank& bank, std::vector<std::string>& warnings, std::string& error) {
    if (!root.is_object() || !root.contains("presets") || !root["presets"].is_object()) {
        error = "presets.json: missing \"presets\" object";
        return false;
    }
    jsonutil::checkKeys(root, {"version", "name", "startup", "presets"}, "presets.json", warnings);
    if (!root.contains("version") || root["version"] != 1) warnings.push_back("presets.json: \"version\" should be 1");
    if (root.contains("name") && root["name"].is_string()) bank.setlistName = root["name"].get<std::string>();

    bank.presets.fill(nullptr);
    for (const auto& item : root["presets"].items()) {
        int slot = 0;
        try {
            std::size_t used = 0;
            slot = std::stoi(item.key(), &used);
            if (used != item.key().size()) slot = 0;
        } catch (const std::exception&) {
            slot = 0;
        }
        if (slot < 1 || slot > kPresetCount) {
            warnings.push_back("presets.json: \"" + item.key() + "\" is not a preset number 1..128 - ignored");
            continue;
        }
        const json& value = item.value();
        if (value.is_null()) continue;
        if (!value.is_string()) {
            warnings.push_back("preset " + item.key() + ": must be a program id or null - left empty");
            continue;
        }
        const Program* program = bank.find(value.get<std::string>());
        if (!program) {
            warnings.push_back("preset " + item.key() + ": program " + value.get<std::string>() +
                               " not available - left empty");
            continue;
        }
        bank.presets[slot - 1] = program;
    }

    bank.startupSlot = 0;
    if (root.contains("startup")) {
        const json& s = root["startup"];
        if (s.is_number_integer() && s.get<int>() >= 1 && s.get<int>() <= kPresetCount && bank.presets[s.get<int>() - 1])
            bank.startupSlot = s.get<int>();
        else
            warnings.push_back("presets.json: \"startup\" is not a defined preset - using the first one");
    }
    if (bank.startupSlot == 0) {
        for (int i = 0; i < kPresetCount; ++i) {
            if (bank.presets[i]) {
                bank.startupSlot = i + 1;
                break;
            }
        }
    }
    return true;
}

bool loadProgramsFile(const std::filesystem::path& file, const InstrumentStore& store, ProgramBank& bank,
                      std::vector<std::string>& warnings, std::string& error, int maxLayers) {
    json root;
    if (!jsonutil::readFile(file, root, error)) return false;
    return loadPrograms(root, store, bank, warnings, error, maxLayers);
}

bool loadPresetsFile(const std::filesystem::path& file, ProgramBank& bank, std::vector<std::string>& warnings,
                     std::string& error) {
    json root;
    if (!jsonutil::readFile(file, root, error)) return false;
    return loadPresets(root, bank, warnings, error);
}

} // namespace mbx
