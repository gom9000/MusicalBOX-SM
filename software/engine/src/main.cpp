// MusicalBOX SM engine
//
// Loads the instruments (instruments.json), the programs (programs.json) and
// the preset table (presets.json), then plays the selected preset from every
// MIDI input found. Presets change with MIDI Program Change 0..15 or from the
// terminal. With --instrument it plays a single instrument instead (test
// mode); with --render it writes a WAV instead of using the audio device;
// with --check it only validates the configuration.

#include "app/Console.h"
#include "app/OfflineRenderer.h"
#include "audio/AudioDevice.h"
#include "config/ConfigLoader.h"
#include "config/EngineConfig.h"
#include "config/ProgramConfig.h"
#include "core/Engine.h"
#include "core/Log.h"
#include "app/BankManager.h"
#include "app/ControlApi.h"
#include "app/ControlServer.h"
#include "app/LiveStats.h"
#include "app/OutputRouter.h"
#include "gpio/GpioLines.h"
#include "app/Recording.h"
#include "config/JsonUtil.h"
#include "midi/RtMidiInput.h"
#include "midi/UartMidiInput.h"
#include "sampler/InstrumentStore.h"
#include "sampler/Program.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#ifdef __linux__
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifndef MBX_VERSION
#define MBX_VERSION "dev"
#endif

namespace {

using namespace mbx;

std::atomic<bool> g_quit{false};

void onSignal(int) {
    g_quit = true;
}

struct Options {
    std::string configDir = "config";
    int preset = 0;          // 0 = the "startup" preset of presets.json
    std::string instrument;  // test mode
    // Values that override engine.json when given on the command line.
    std::optional<std::string> device;
    std::optional<unsigned> sampleRate;
    std::optional<unsigned> periodFrames;
    std::optional<unsigned> periods;
    std::optional<float> volumeDb;
    std::optional<int> polyphony;
    std::optional<long> ramBudgetMb;
    std::optional<std::string> midiSerial;
    std::string recordPath;
    Adsr adsr;
    bool loop = false;
    int velocityLevels = 0;  // 0 = velocity off
    VelocityCurve curve = VelocityCurve::Linear;
    std::string renderPath;
    std::string renderNotes = "60@0:2";
    double renderTail = 2.0;
    bool stats = false;
    bool check = false;
    bool listDevices = false;
    bool listMidi = false;
};

void printUsage() {
    std::printf(
        "mbx-engine %s - MusicalBOX SM sampler engine\n"
        "\n"
        "Usage: mbx-engine [options]\n"
        "\n"
        "  --config DIR          folder with engine.json, instruments.json, programs.json,\n"
        "                        presets.json (default: config)\n"
        "  --preset N            preset to start with, 1..128 (default: \"startup\" in presets.json)\n"
        "  --check               load and validate the configuration, then exit\n"
        "\n"
        "These override engine.json:\n"
        "  --volume DB           master volume in dB (engine.json default: -6)\n"
        "  --polyphony N         voices (engine.json default: 64)\n"
        "  --ram-budget MB       sample memory limit, 0 = none (engine.json default: 70%% of RAM)\n"
        "  --device NAME         output device, by part of its name (default: system default);\n"
        "                        \"null\" = no sound card (tests)\n"
        "  --rate HZ             sample rate (default: 48000)\n"
        "  --period FRAMES       audio period (default: 128)\n"
        "  --periods N           number of periods (default: 3)\n"
        "  --midi-serial DEV     serial port of the DIN MIDI input, \"\" = none (default: /dev/serial0)\n"
        "\n"
        "Test mode, one instrument without programs.json:\n"
        "  --instrument ID       instrument to play\n"
        "  --adsr A,D,S,R        envelope: attack ms, decay ms, sustain 0..1, release ms\n"
        "  --loop                organ mode: hold notes on the WAV loop points\n"
        "  --velocity N          velocity on, quantised to N levels\n"
        "  --curve linear|exp    velocity curve\n"
        "\n"
        "Diagnostics:\n"
        "  --stats               print voices and audio load every 5 s\n"
        "  --record FILE.wav     record what is played (also: r in the console)\n"
        "  --list-devices        list audio outputs and exit\n"
        "  --list-midi           list MIDI inputs and exit\n"
        "\n"
        "Render to file:\n"
        "  --render FILE.wav     render to a WAV file instead of playing\n"
        "  --notes LIST          comma-separated, times in seconds:\n"
        "                          60@0:2       note 60 at 0 s for 2 s\n"
        "                          64@0.5:1:80  ... with velocity 80\n"
        "                          p3@1.5       select preset 3 at 1.5 s\n"
        "  --tail SECONDS        audio rendered after the last event (default: 2)\n"
        "\n"
        "While playing, type in the terminal: 1..128 preset, v <dB> volume,\n"
        "x all sound off, s status, q quit.\n"
        "\n"
        "  --version, --help\n",
        MBX_VERSION);
}

bool parseAdsr(const std::string& text, Adsr& adsr) {
    std::stringstream ss(text);
    std::string part;
    float v[4];
    int n = 0;
    while (std::getline(ss, part, ',') && n < 4) {
        try {
            v[n++] = std::stof(part);
        } catch (const std::exception&) {
            return false;
        }
    }
    if (n != 4 || v[0] < 0 || v[1] < 0 || v[2] < 0 || v[2] > 1 || v[3] < 0) return false;
    adsr = {v[0], v[1], v[2], v[3]};
    return true;
}

// Returns 0 to continue, otherwise the exit code (+1).
int parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                log::error("%s needs a value", name);
                std::exit(2);
            }
            return argv[++i];
        };
        try {
            if (a == "--help" || a == "-h") {
                printUsage();
                return 1;
            } else if (a == "--version") {
                std::printf("%s\n", MBX_VERSION);
                return 1;
            } else if (a == "--config") {
                o.configDir = value("--config");
            } else if (a == "--preset") {
                o.preset = std::stoi(value("--preset"));
                if (o.preset < 1 || o.preset > kPresetCount) {
                    log::error("--preset must be 1..128");
                    return 3;
                }
            } else if (a == "--instrument") {
                o.instrument = value("--instrument");
            } else if (a == "--volume") {
                o.volumeDb = std::stof(value("--volume"));
            } else if (a == "--polyphony") {
                o.polyphony = std::stoi(value("--polyphony"));
            } else if (a == "--adsr") {
                if (!parseAdsr(value("--adsr"), o.adsr)) {
                    log::error("--adsr expects A,D,S,R (ms, ms, 0..1, ms)");
                    return 3;
                }
            } else if (a == "--loop") {
                o.loop = true;
            } else if (a == "--velocity") {
                o.velocityLevels = std::stoi(value("--velocity"));
            } else if (a == "--curve") {
                const std::string c = value("--curve");
                if (c == "linear") {
                    o.curve = VelocityCurve::Linear;
                } else if (c == "exp") {
                    o.curve = VelocityCurve::Exp;
                } else {
                    log::error("--curve must be linear or exp");
                    return 3;
                }
            } else if (a == "--ram-budget") {
                o.ramBudgetMb = std::stol(value("--ram-budget"));
            } else if (a == "--check") {
                o.check = true;
            } else if (a == "--device") {
                o.device = value("--device");
            } else if (a == "--rate") {
                o.sampleRate = static_cast<unsigned>(std::stoul(value("--rate")));
            } else if (a == "--period") {
                o.periodFrames = static_cast<unsigned>(std::stoul(value("--period")));
            } else if (a == "--periods") {
                o.periods = static_cast<unsigned>(std::stoul(value("--periods")));
            } else if (a == "--record") {
                o.recordPath = value("--record");
            } else if (a == "--midi-serial") {
                o.midiSerial = value("--midi-serial");
            } else if (a == "--stats") {
                o.stats = true;
            } else if (a == "--list-devices") {
                o.listDevices = true;
            } else if (a == "--list-midi") {
                o.listMidi = true;
            } else if (a == "--render") {
                o.renderPath = value("--render");
            } else if (a == "--notes") {
                o.renderNotes = value("--notes");
            } else if (a == "--tail") {
                o.renderTail = std::stod(value("--tail"));
            } else {
                log::error("unknown option %s (see --help)", a.c_str());
                return 3;
            }
        } catch (const std::exception&) {
            log::error("invalid value for %s", a.c_str());
            return 3;
        }
    }
    return 0;
}

// Sample memory limit in bytes, 0 = none: --ram-budget MB if given,
// otherwise ram_budget_percent of the physical RAM.
std::size_t ramBudgetBytes(std::optional<long> mb, int percent) {
    if (mb) return *mb > 0 ? static_cast<std::size_t>(*mb) << 20 : 0;
    if (percent <= 0) return 0;
#ifdef __linux__
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0)
        return static_cast<std::size_t>(static_cast<double>(pages) * static_cast<double>(pageSize) * percent / 100.0);
#endif
    return 0;
}

// Keeps samples and thread stacks in RAM (no paging during a gig). Called once,
// after every thread has started: only what exists now is locked (no
// MCL_FUTURE), so a later thread or allocation can never fail because of it.
void lockMemory() {
#ifdef __linux__
    if (mlockall(MCL_CURRENT) == 0) return;
    const int err = errno;
    struct rlimit limit {};
    getrlimit(RLIMIT_MEMLOCK, &limit);
    if (limit.rlim_cur == RLIM_INFINITY)
        log::warn("mlockall failed (%s): samples may be paged out", std::strerror(err));
    else
        log::warn("mlockall failed (%s): the memory lock limit is %llu MB, too little for the samples - "
                  "they may be paged out. Raise it: \"ulimit -l\", LimitMEMLOCK in the systemd unit "
                  "or memlock in /etc/security/limits.conf",
                  std::strerror(err), static_cast<unsigned long long>(limit.rlim_cur / (1024 * 1024)));
#endif
}

std::vector<std::string> g_startupWarnings;  // for the control socket ("warnings")

void flush(std::vector<std::string>& warnings) {
    for (const auto& w : warnings) {
        log::warn("%s", w.c_str());
        g_startupWarnings.push_back(w);
    }
    warnings.clear();
}

// "-12.3 dBFS", or "-inf" when silent. Above 0 dBFS the limiter has worked.
std::string peakText(float peak) {
    if (peak < 1e-6f) return "  -inf dBFS";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+6.1f dBFS", 20.0f * std::log10(peak));
    return buf;
}

std::string limiterText(float minGain) {
    if (minGain > 0.999f) return "";
    char buf[48];
    std::snprintf(buf, sizeof buf, " (limiter %.1f dB)", 20.0f * std::log10(minGain));
    return buf;
}

std::string describe(const Program* p) {
    if (!p) return "-";
    std::string s = p->name + " (" + p->id + "), " + std::to_string(p->layers.size()) + " layer" +
                    (p->layers.size() == 1 ? "" : "s") + ", output " + outputName(p->output);
    std::string fx, sw;
    for (const auto& d : p->fx) {
        if (d.bypass) continue;
        fx += (fx.empty() ? "" : " > ") + d.type;
        if (!d.switchParam.empty()) sw += (sw.empty() ? "" : ", ") + d.type + " " + d.switchParam;
    }
    if (!fx.empty()) s += ", fx " + fx;
    if (!sw.empty()) s += " (switch: " + sw + ")";
    return s;
}

// The program built from the command line in test mode.
Program testProgram(const Instrument* instrument, const Options& opt) {
    Program program;
    program.id = "test";
    program.name = instrument->name();
    Layer layer;
    layer.instrument = instrument;
    layer.adsr = opt.adsr;
    layer.loop = opt.loop;
    if (opt.velocityLevels > 0) {
        layer.velocity.enabled = true;
        layer.velocity.levels = opt.velocityLevels;
        layer.velocity.curve = opt.curve;
    }
    program.layers.push_back(layer);
    return program;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (int code = parseArgs(argc, argv, opt)) return code - 1;

    if (opt.listDevices || opt.listMidi) {
        if (opt.listDevices) AudioDevice::listDevices();
        if (opt.listMidi) {
            RtMidiInput::listPorts();
            EngineSettings s;
            bool found = false;
            std::vector<std::string> ignoredWarnings;
            std::string ignoredError;
            loadEngineSettings(std::filesystem::path(opt.configDir) / "engine.json", s, found, ignoredWarnings,
                               ignoredError);
            UartMidiInput::describe(opt.midiSerial ? *opt.midiSerial : s.midiSerial);
        }
        return 0;
    }

    log::info("mbx-engine %s", MBX_VERSION);
    const std::filesystem::path configDir = opt.configDir;
    std::vector<std::string> warnings;
    int warningCount = 0;
    std::string error;

    // --- System settings: engine.json, then the command line ----------------
    EngineSettings settings;
    bool settingsFound = false;
    if (!loadEngineSettings(configDir / "engine.json", settings, settingsFound, warnings, error)) {
        log::error("%s", error.c_str());
        return 1;
    }
    if (!settingsFound) log::info("no engine.json: using the default settings");
    if (opt.device) settings.audioDevice = *opt.device;
    if (opt.sampleRate) settings.sampleRate = *opt.sampleRate;
    if (opt.periodFrames) settings.periodFrames = *opt.periodFrames;
    if (opt.periods) settings.periods = *opt.periods;
    if (opt.volumeDb) settings.masterVolumeDb = *opt.volumeDb;
    if (opt.polyphony) settings.polyphony = *opt.polyphony;
    if (opt.midiSerial) settings.midiSerial = *opt.midiSerial;
    warningCount += static_cast<int>(warnings.size());
    flush(warnings);
    log::info("settings: %d voices, max %d layers per program, volume %.1f dB, MIDI channel %s, DIN %s",
              settings.polyphony, settings.maxLayers, settings.masterVolumeDb,
              settings.midiChannel ? std::to_string(settings.midiChannel).c_str() : "omni",
              settings.midiSerial.empty() ? "off" : settings.midiSerial.c_str());

    // --- DAC mute pins: low (muted) from here until the audio runs -----------
    GpioLines dacPins;
    if (settings.dacMute.enabled) {
        std::string msg;
        const auto& d = settings.dacMute;
        switch (dacPins.open(d.chip, {static_cast<unsigned>(d.a), static_cast<unsigned>(d.b)}, "mbx-engine dac mute",
                             msg)) {
        case GpioLines::Result::Ok:
            log::info("DAC outputs: A = GPIO%d, B = GPIO%d (%s)", d.a, d.b, dacPins.chipPath().c_str());
            break;
        case GpioLines::Result::NoChip:
            log::info("DAC outputs: %s - A/B not driven", msg.c_str());
            break;
        case GpioLines::Result::Failed:
            warnings.push_back("dac_mute: cannot drive GPIO" + std::to_string(d.a) + "/" + std::to_string(d.b) + " (" +
                               msg + ") - a DAC with XSMT on these pins stays muted");
            break;
        }
        warningCount += static_cast<int>(warnings.size());
        flush(warnings);
    }

    // --- Instruments ---------------------------------------------------------
    InstrumentsConfig config;
    if (!loadInstrumentsConfig(configDir / "instruments.json", config, warnings, error)) {
        log::error("%s", error.c_str());
        return 1;
    }
    log::info("media folder: %s", config.mediaRoot.string().c_str());

    InstrumentStore store;
    store.setBudgetBytes(ramBudgetBytes(opt.ramBudgetMb, settings.ramBudgetPercent));
    const auto t0 = std::chrono::steady_clock::now();
    store.loadAll(config, warnings);
    const double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    warningCount += static_cast<int>(warnings.size());
    flush(warnings);
    const std::size_t systemWarningCount = g_startupWarnings.size();  // settings + instruments

    for (const auto& inst : store.all()) {
        std::string notes = inst->hasLoops() ? "  (loops)" : "";
        if (const int n = inst->dualMonoCount())
            notes += "  (" + std::string(n == static_cast<int>(inst->zoneCount()) ? "all" : std::to_string(n)) +
                     " stereo with identical channels: kept as mono)";
        log::info("instrument %-16s %4zu files %8.1f MB%s", inst->id().c_str(), inst->zoneCount(),
                  inst->memoryBytes() / 1048576.0, notes.c_str());
    }
    log::info("%zu instruments, %.1f MB in %.1f s", store.all().size(), store.totalBytes() / 1048576.0,
              loadSeconds);
    if (store.all().empty()) {
        log::error("no instrument loaded: check instruments.json and the media folder");
        return 1;
    }

    auto engine = std::make_unique<Engine>();
    engine->setMasterGainDb(settings.masterVolumeDb);
    engine->setPolyphony(settings.polyphony);
    engine->setMidiChannel(settings.midiChannel);
    engine->setPitchBendRange(settings.pitchBendRange);
    BankManager banks(*engine, store, settings.maxLayers);
    std::shared_ptr<const ProgramBank> startBank;
    Program test;
    const bool testMode = !opt.instrument.empty();

    if (testMode) {
        // --- Test mode: one instrument ---------------------------------------
        const Instrument* instrument = store.find(opt.instrument);
        if (!instrument) {
            log::error("instrument %s not loaded", opt.instrument.c_str());
            return 1;
        }
        if (opt.loop && !instrument->hasLoops())
            log::warn("--loop: %s has no loop points", instrument->id().c_str());
        test = testProgram(instrument, opt);
        engine->setProgram(&test);
        log::info("test mode: %s", describe(&test).c_str());
    } else {
        // --- Programs and presets ---------------------------------------------
        nlohmann::json programsJson, presetsJson;
        std::shared_ptr<ProgramBank> bank;
        if (jsonutil::readFile(configDir / "programs.json", programsJson, error) &&
            jsonutil::readFile(configDir / "presets.json", presetsJson, error))
            bank = banks.build(programsJson, presetsJson, warnings, error);
        if (!bank) {
            flush(warnings);
            log::error("%s", error.c_str());
            if (error == "no preset is usable") log::error("check presets.json and programs.json");
            log::error("to play a single instrument without programs, use --instrument ID");
            return 1;
        }
        warningCount += static_cast<int>(warnings.size());
        flush(warnings);

        log::info("%zu programs; presets%s%s:", bank->programs.size(), bank->setlistName.empty() ? "" : " of ",
                  bank->setlistName.c_str());
        for (int slot = 1; slot <= kPresetCount; ++slot)
            if (bank->presets[slot - 1]) log::info("  %2d  %s", slot, describe(bank->presets[slot - 1]).c_str());

        banks.install(bank);
        startBank = bank;
        const int first = opt.preset ? opt.preset : bank->startupSlot;
        if (!engine->selectPresetNow(first)) {
            log::error("preset %d is empty", first);
            return 1;
        }
    }

    if (opt.check) {
        if (warningCount == 0) {
            log::info("configuration OK");
            return 0;
        }
        log::warn("configuration loaded with %d warning(s)", warningCount);
        return 1;
    }

    // --- Offline render ------------------------------------------------------
    if (!opt.renderPath.empty()) {
        std::vector<ScheduledEvent> events;
        if (!parseEventList(opt.renderNotes, events, error)) {
            log::error("--notes: %s", error.c_str());
            return 1;
        }
        engine->prepare(settings.sampleRate);
        if (!renderOffline(*engine, events, opt.renderTail, opt.renderPath, error)) {
            log::error("%s", error.c_str());
            return 1;
        }
        log::info("rendered %zu events to %s", events.size(), opt.renderPath.c_str());
        return 0;
    }

    // --- Live ----------------------------------------------------------------
    AudioOptions audioOptions;
    audioOptions.deviceName = settings.audioDevice;
    audioOptions.sampleRate = settings.sampleRate;
    audioOptions.periodFrames = settings.periodFrames;
    audioOptions.periods = settings.periods;
    AudioDevice audio;
    if (!audio.open(*engine, audioOptions, error)) {
        log::error("%s", error.c_str());
        return 1;
    }
    if (!audio.start(error)) {
        log::error("%s", error.c_str());
        return 1;
    }
    const double latencyMs = 1000.0 * audio.periodFrames() * settings.periods / audio.sampleRate();
    log::info("audio: %s, %u Hz, period %u frames (buffer about %.1f ms)", audio.deviceName().c_str(),
              audio.sampleRate(), audio.periodFrames(), latencyMs);

    // Program output A / B / AB -> DAC mute pins, following the sound into the DACs.
    OutputRouter outputs(*engine, [&](bool a, bool b) { dacPins.set((a ? 1u : 0u) | (b ? 2u : 0u)); });
    if (dacPins.isOpen()) outputs.start(latencyMs);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    RtMidiInput midiIn(*engine);
    midiIn.scan();
    UartMidiInput dinIn(*engine, settings.midiSerial);
    dinIn.scan();

    auto presetProgram = [&](int slot) -> const Program* {
        const auto bank = banks.current();
        return bank && slot >= 1 && slot <= kPresetCount ? bank->presets[slot - 1] : nullptr;
    };
    auto currentProgram = [&]() -> const Program* {
        const int slot = engine->currentSlot();
        return slot ? presetProgram(slot) : &test;
    };

    Recording recording(*engine, settings.recordFolder);
    if (!opt.recordPath.empty() && !recording.start(opt.recordPath, error)) log::warn("%s", error.c_str());

    // Control socket: web editor (mbx-control), later the front panel.
    LiveStats liveStats;
    std::unique_ptr<ControlApi> controlApi;
    ControlServer control;
    if (!testMode && !settings.controlSocket.empty()) {
        controlApi = std::make_unique<ControlApi>(*engine, banks, store, configDir, liveStats, g_startupWarnings,
                                                  systemWarningCount,
                                                  MBX_VERSION);
        controlApi->setRecording(&recording);
        if (control.start(settings.controlSocket, [&](const std::string& r) { return controlApi->handle(r); }, error))
            log::info("control socket: %s", settings.controlSocket.c_str());
        else
            log::warn("%s - no web editor", error.c_str());
    }
    startBank.reset();  // from here on the bank manager decides
    auto status = [&]() {
        const int slot = engine->currentSlot();
        log::info("preset %d: %s - voices %d", slot, describe(currentProgram()).c_str(), engine->activeVoices());
    };
    Console console(*engine, testMode ? Console::PresetLookup() : Console::PresetLookup(presetProgram), status,
                    [] { g_quit = true; }, [&] {
                        if (recording.status().recording) {
                            recording.stop();
                        } else if (std::string e; !recording.start("", e)) {
                            log::warn("%s", e.c_str());
                        }
                    });
    console.start();
    lockMemory();

    int lastSlot = -1;
    bool lastSwitch = false;
    auto lastScan = std::chrono::steady_clock::now();
    auto lastStats = lastScan;
    auto lastSample = lastScan;
    const auto started = lastScan;
    LiveStats::Snapshot sum;  // --stats: over 5 s
    int samples = 0;
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const int slot = engine->currentSlot();
        const bool switchB = engine->fxSwitchB();
        if (switchB != lastSwitch && slot == lastSlot) log::info("effect switch: %s", switchB ? "B" : "A");
        lastSwitch = switchB;
        if (slot != lastSlot) {
            if (slot) log::info("preset %d: %s", slot, describe(currentProgram()).c_str());
            else log::info("playing %s - Ctrl+C to quit", describe(currentProgram()).c_str());
            lastSlot = slot;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto scanEvery = std::chrono::seconds(midiIn.systemAvailable() ? 2 : 30);
        if (now - lastScan >= scanEvery) {
            midiIn.scan();
            dinIn.scan();
            lastScan = now;
        }
        if (now - lastSample >= std::chrono::seconds(1)) {
            // Once a second: figures for the control socket, summed up for --stats.
            const auto load = audio.takeLoadStats();
            const auto level = engine->takeLevelStats();
            LiveStats::Snapshot snap;
            snap.loadAverage = load.averageLoad;
            snap.loadMax = load.maxLoad;
            snap.peak = level.peak;
            snap.limiterGain = level.minLimiterGain;
            snap.maxVoices = level.maxVoices;
            snap.uptimeSeconds = std::chrono::duration<double>(now - started).count();
            liveStats.set(snap);
            sum.loadAverage += load.averageLoad;
            sum.loadMax = std::max(sum.loadMax, load.maxLoad);
            sum.peak = std::max(sum.peak, level.peak);
            sum.limiterGain = std::min(sum.limiterGain, level.minLimiterGain);
            sum.maxVoices = std::max(sum.maxVoices, level.maxVoices);
            ++samples;
            lastSample = now;
            banks.collect();
        }
        if (opt.stats && now - lastStats >= std::chrono::seconds(5) && samples > 0) {
            log::info("voices %2d (max %2d)   load avg %4.1f%% max %5.1f%%   peak %s%s   dropped events %d",
                      engine->activeVoices(), sum.maxVoices, 100.0f * sum.loadAverage / samples, 100.0f * sum.loadMax,
                      peakText(sum.peak).c_str(), limiterText(sum.limiterGain).c_str(), engine->droppedEvents());
            sum = LiveStats::Snapshot();
            samples = 0;
            lastStats = now;
        }
    }

    log::info("stopping");
    control.stop();
    recording.stop();
    midiIn.close();
    dinIn.close();
    outputs.stop();  // muted (with the DACs' ramp) before the clock stops
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    audio.stop();
    return 0;
}
