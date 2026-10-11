#pragma once

#include "app/LiveStats.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mbx {

class BankManager;
class Engine;
class InstrumentStore;
class Recording;

// The commands of the control socket. One request in, one reply out, both
// JSON objects: {"cmd": "status"} -> {"ok": true, ...}. See README, "Socket di
// controllo", for the list. Runs on the control server thread, which owns
// its own engine source.
class ControlApi {
public:
    ControlApi(Engine& engine, BankManager& banks, const InstrumentStore& store, std::filesystem::path configDir,
               const LiveStats& stats, std::vector<std::string> startupWarnings, std::size_t systemWarnings,
               std::string version);
    ~ControlApi();

    std::string handle(const std::string& request);
    void setRecording(Recording* recording) { recording_ = recording; }

private:
    Engine& engine_;
    BankManager& banks_;
    const InstrumentStore& store_;
    std::filesystem::path configDir_;
    const LiveStats& stats_;
    std::vector<std::string> startupWarnings_;
    std::vector<std::string> lastWarnings_;  // of the last preview / reload
    // In force now: settings and instruments (fixed until restart) + programs and
    // presets as loaded at startup or by the last reload. For the status LED.
    std::vector<std::string> systemWarnings_, fileWarnings_;
    std::string version_;
    bool edited_ = false;                    // playing a preview, not the files
    int source_ = -1;
    Recording* recording_ = nullptr;
};

} // namespace mbx
