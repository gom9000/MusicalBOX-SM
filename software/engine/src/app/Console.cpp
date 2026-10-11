#include "app/Console.h"
#include "fx/FxChain.h"
#include <system_error>

#include "core/Engine.h"
#include "core/Log.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define MBX_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define MBX_ISATTY(fd) isatty(fd)
#endif

namespace mbx {

Console::Console(Engine& engine, PresetLookup presets, std::function<void()> onStatus,
                 std::function<void()> onQuit, std::function<void()> onRecord)
    : engine_(engine),
      presets_(presets),
      onStatus_(std::move(onStatus)),
      onQuit_(std::move(onQuit)),
      onRecord_(std::move(onRecord)) {}

Console::~Console() {
    // The thread may be blocked reading stdin: it is detached, never joined.
    if (thread_.joinable()) thread_.detach();
}

bool Console::start() {
    if (!MBX_ISATTY(0)) return false;
    source_ = engine_.registerSource();
    if (source_ < 0) return false;
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error& e) {
        log::warn("console not available: %s", e.what());
        engine_.releaseSource(source_);
        source_ = -1;
        return false;
    }
    return true;
}

void Console::run() {
    std::printf("commands: 1..128 preset, w effect switch, v <dB> volume, x all sound off, r record, s status, q quit\n");
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        if (!(in >> cmd)) continue;

        if (cmd == "q" || cmd == "quit") {
            onQuit_();
            return;
        }
        if (cmd == "s") {
            onStatus_();
        } else if (cmd == "r") {
            if (onRecord_) onRecord_();
        } else if (cmd == "w") {
            const int slot = engine_.currentSlot();
            const Program* p = (presets_ && slot >= 1) ? presets_(slot) : nullptr;
            if (p && !(p->fxChain && p->fxChain->hasSwitch()))
                log::warn("preset %d has no effect with a \"switch\"", slot);
            else
                engine_.post(source_, EngineEvent::fxSwitch());
        } else if (cmd == "x") {
            engine_.post(source_, EngineEvent::allSoundOff());
        } else if (cmd == "v") {
            float db = 0.0f;
            if (in >> db && db <= 6.0f && db >= -90.0f) {
                engine_.post(source_, EngineEvent::masterGainDb(db));
                log::info("volume %.1f dB", db);
            } else {
                log::warn("usage: v <dB>, from -90 to 6");
            }
        } else {
            int slot = 0;
            try {
                std::size_t used = 0;
                slot = std::stoi(cmd, &used);
                if (used != cmd.size()) slot = 0;
            } catch (const std::exception&) {
                slot = 0;
            }
            if (slot >= 1 && slot <= kPresetCount) {
                if (!presets_)
                    log::warn("test mode (--instrument): there are no presets");
                else if (!presets_(slot))
                    log::warn("preset %d is empty", slot);
                else
                    engine_.post(source_, EngineEvent::selectPreset(slot));
            } else
                log::warn("unknown command \"%s\"", line.c_str());
        }
    }
}

} // namespace mbx
