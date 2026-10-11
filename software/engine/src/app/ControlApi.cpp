#include "app/ControlApi.h"

#include "app/BankManager.h"
#include "app/Recording.h"
#include "config/JsonUtil.h"
#include "core/Log.h"
#include "core/Engine.h"
#include "fx/FxChain.h"
#include "sampler/Instrument.h"
#include "sampler/InstrumentStore.h"

#include <cmath>

namespace mbx {

using nlohmann::json;

namespace {

json fail(const std::string& error) {
    return {{"ok", false}, {"error", error}};
}

double db(float linear) {
    return linear > 1e-6f ? std::round(200.0 * std::log10(linear)) / 10.0 : -120.0;
}

json describeBank(const ProgramBank& bank) {
    json programs = json::array();
    for (const auto& p : bank.programs) {
        json layers = json::array(), fx = json::array();
        for (const auto& l : p->layers) layers.push_back(l.instrument ? l.instrument->id() : "");
        for (const auto& f : p->fx) fx.push_back(f.type);
        programs.push_back({{"id", p->id},
                            {"name", p->name},
                            {"layers", layers},
                            {"fx", fx},
                            {"switch", p->fxChain && p->fxChain->hasSwitch()}});
    }
    json presets = json::array();
    for (const Program* p : bank.presets) presets.push_back(p ? json(p->id) : json(nullptr));
    return {{"name", bank.setlistName}, {"startup", bank.startupSlot}, {"programs", programs}, {"presets", presets}};
}

} // namespace

ControlApi::ControlApi(Engine& engine, BankManager& banks, const InstrumentStore& store,
                       std::filesystem::path configDir, const LiveStats& stats,
                       std::vector<std::string> startupWarnings, std::size_t systemWarnings,
                       std::string version)
    : engine_(engine),
      banks_(banks),
      store_(store),
      configDir_(std::move(configDir)),
      stats_(stats),
      startupWarnings_(std::move(startupWarnings)),
      systemWarnings_(startupWarnings_.begin(),
                      startupWarnings_.begin() + static_cast<long>(std::min(systemWarnings, startupWarnings_.size()))),
      fileWarnings_(startupWarnings_.begin() + static_cast<long>(systemWarnings_.size()), startupWarnings_.end()),
      version_(std::move(version)) {}

ControlApi::~ControlApi() {
    if (source_ >= 0) engine_.releaseSource(source_);
}

std::string ControlApi::handle(const std::string& request) {
    // The source belongs to the thread that calls handle(): registered on first use.
    if (source_ < 0) source_ = engine_.registerSource();
    if (source_ < 0) return fail("engine: no free event source").dump();

    json req;
    try {
        req = json::parse(request);
    } catch (const json::parse_error& e) {
        return fail(std::string("request is not JSON: ") + e.what()).dump();
    }
    const std::string cmd = req.value("cmd", "");

    try {
        if (cmd == "status") {
            const auto s = stats_.get();
            const int slot = engine_.currentSlot();
            const auto bank = banks_.current();
            json program = nullptr;
            bool hasSwitch = false;
            if (bank && slot >= 1) {
                if (const Program* p = bank->presets[slot - 1]) {
                    program = {{"id", p->id}, {"name", p->name}};
                    hasSwitch = p->fxChain && p->fxChain->hasSwitch();
                }
            }
            return json{{"ok", true},
                        {"version", version_},
                        {"slot", slot},
                        {"program", program},
                        {"switch", engine_.fxSwitchB()},
                        {"has_switch", hasSwitch},
                        {"voices", engine_.activeVoices()},
                        {"max_voices", s.maxVoices},
                        {"polyphony", engine_.polyphony()},
                        {"load_avg", std::round(s.loadAverage * 1000.0) / 10.0},
                        {"load_max", std::round(s.loadMax * 1000.0) / 10.0},
                        {"peak_db", db(s.peak)},
                        {"limiter_db", db(s.limiterGain)},
                        {"volume_db", engine_.masterGainDb()},
                        {"midi_messages", engine_.midiMessages()},
                        {"dropped", engine_.droppedEvents()},
                        {"edited", edited_},
                        {"uptime_s", std::round(s.uptimeSeconds)},
                        {"recording", recording_ && recording_->status().recording}}
                .dump();
        }
        if (cmd == "bank") {
            const auto bank = banks_.current();
            if (!bank) return fail("no programs loaded (test mode)").dump();
            json reply = describeBank(*bank);
            reply["ok"] = true;
            reply["edited"] = edited_;
            return reply.dump();
        }
        if (cmd == "instruments") {
            json list = json::array();
            for (const auto& inst : store_.all())
                list.push_back({{"id", inst->id()},
                                {"name", inst->name()},
                                {"files", inst->zoneCount()},
                                {"mb", std::round(inst->memoryBytes() / 104857.6) / 10.0},
                                {"loops", inst->hasLoops()},
                                {"dual_mono", inst->dualMonoCount()}});
            return json{{"ok", true}, {"instruments", list}, {"total_mb", std::round(store_.totalBytes() / 104857.6) / 10.0}}
                .dump();
        }
        if (cmd == "record") {
            if (!recording_) return fail("recording not available").dump();
            const std::string action = req.value("action", "status");
            std::string error;
            if (action == "start" && !recording_->start(req.value("path", ""), error)) return fail(error).dump();
            std::string written;
            if (action == "stop") written = recording_->stop();
            const auto st = recording_->status();
            json reply{{"ok", true},
                       {"recording", st.recording},
                       {"path", st.recording ? st.path : written},
                       {"seconds", std::round(st.seconds * 10.0) / 10.0},
                       {"dropped_frames", st.dropped},
                       {"failed", st.failed},
                       {"folder", recording_->folder()}};
            return reply.dump();
        }
        if (cmd == "warnings") {
            std::vector<std::string> active = systemWarnings_;
            active.insert(active.end(), fileWarnings_.begin(), fileWarnings_.end());
            return json{{"ok", true}, {"startup", startupWarnings_}, {"last", lastWarnings_}, {"active", active}}.dump();
        }
        if (cmd == "preset") {
            const int slot = req.value("slot", 0);
            if (slot < 1 || slot > kPresetCount) return fail("slot must be 1..128").dump();
            const auto bank = banks_.current();
            if (!bank || !bank->presets[slot - 1]) return fail("preset " + std::to_string(slot) + " is empty").dump();
            engine_.post(source_, EngineEvent::selectPreset(slot));
            return json{{"ok", true}}.dump();
        }
        if (cmd == "switch") {
            engine_.post(source_, EngineEvent::fxSwitch(req.value("position", -1)));
            return json{{"ok", true}}.dump();
        }
        if (cmd == "volume") {
            const float v = req.value("db", 0.0f);
            if (!(v >= -90.0f && v <= 6.0f)) return fail("db must be -90..6").dump();
            engine_.post(source_, EngineEvent::masterGainDb(v));
            return json{{"ok", true}}.dump();
        }
        if (cmd == "panic") {
            engine_.post(source_, EngineEvent::allSoundOff());
            return json{{"ok", true}}.dump();
        }
        if (cmd == "note") {
            const int key = req.value("key", -1), velocity = req.value("velocity", 100);
            if (key < 0 || key > 127 || velocity < 0 || velocity > 127) return fail("key 0..127, velocity 0..127").dump();
            const int channel = engine_.midiChannel() ? engine_.midiChannel() - 1 : 0;
            MidiEvent m;
            m.status = static_cast<std::uint8_t>((velocity > 0 ? midi::kNoteOn : midi::kNoteOff) | channel);
            m.data1 = static_cast<std::uint8_t>(key);
            m.data2 = static_cast<std::uint8_t>(velocity);
            engine_.post(source_, EngineEvent::fromMidi(m));
            return json{{"ok", true}}.dump();
        }
        if (cmd == "preview" || cmd == "reload") {
            if (!banks_.current()) return fail("no programs loaded (test mode)").dump();
            json programs, presets;
            std::string error;
            if (cmd == "preview") {
                if (!req.contains("programs") || !req.contains("presets"))
                    return fail("preview needs \"programs\" and \"presets\"").dump();
                programs = req["programs"];
                presets = req["presets"];
            } else if (!jsonutil::readFile(configDir_ / "programs.json", programs, error) ||
                       !jsonutil::readFile(configDir_ / "presets.json", presets, error)) {
                return fail(error).dump();
            }
            std::vector<std::string> warnings;
            auto bank = banks_.build(programs, presets, warnings, error);
            if (!bank) return json{{"ok", false}, {"error", error}, {"warnings", warnings}}.dump();
            if (!banks_.swap(bank, source_)) return fail("engine busy, try again").dump();
            lastWarnings_ = warnings;
            edited_ = cmd == "preview";
            if (cmd == "reload") fileWarnings_ = warnings;
            if (cmd == "reload") {
                log::info("programs and presets reloaded from the files (%zu warning(s))", warnings.size());
                for (const auto& w : warnings) log::warn("%s", w.c_str());
            }
            return json{{"ok", true}, {"warnings", warnings}}.dump();
        }
        if (cmd == "check") {
            // Like preview, without playing it: validation before saving.
            if (!req.contains("programs") || !req.contains("presets"))
                return fail("check needs \"programs\" and \"presets\"").dump();
            std::vector<std::string> warnings;
            std::string error;
            auto bank = banks_.build(req["programs"], req["presets"], warnings, error);
            return json{{"ok", bank != nullptr}, {"error", error}, {"warnings", warnings}}.dump();
        }
    } catch (const std::exception& e) {
        return fail(std::string("bad request: ") + e.what()).dump();
    }
    return fail("unknown command \"" + cmd + "\"").dump();
}

} // namespace mbx
