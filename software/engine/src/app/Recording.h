#pragma once

#include "core/Recorder.h"

#include <memory>
#include <mutex>
#include <string>

namespace mbx {

class Engine;

// Start / stop of the recording of the engine's output, shared by the
// command line (--record), the console (r) and the control socket.
class Recording {
public:
    Recording(Engine& engine, std::string folder) : engine_(engine), folder_(std::move(folder)) {}
    ~Recording() { stop(); }

    // path empty = <folder>/mbx-rec-YYYYMMDD-HHMMSS.wav. False (error set) if it cannot start.
    bool start(std::string path, std::string& error);
    // Returns the file written ("" if nothing was recording).
    std::string stop();

    struct Status {
        bool recording = false;
        std::string path;
        double seconds = 0.0;
        unsigned long long dropped = 0;
        bool failed = false;
    };
    Status status();
    const std::string& folder() const { return folder_; }

private:
    Engine& engine_;
    std::string folder_;
    std::mutex mutex_;
    std::unique_ptr<Recorder> recorder_;
};

} // namespace mbx
