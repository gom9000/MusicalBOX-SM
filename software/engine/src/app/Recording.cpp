#include "app/Recording.h"

#include "core/Engine.h"
#include "core/Log.h"

#include <ctime>
#include <filesystem>

namespace mbx {

bool Recording::start(std::string path, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recorder_) {
        error = "already recording to " + recorder_->path();
        return false;
    }
    if (path.empty()) {
        char name[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(name, sizeof name, "mbx-rec-%Y%m%d-%H%M%S.wav", std::localtime(&now));
        path = (std::filesystem::path(folder_) / name).string();
    }
    auto rec = std::make_unique<Recorder>();
    if (!rec->start(path, static_cast<unsigned>(engine_.sampleRate()), error)) return false;
    engine_.setRecorder(rec.get());
    recorder_ = std::move(rec);
    log::info("recording to %s", path.c_str());
    return true;
}

std::string Recording::stop() {
    std::unique_ptr<Recorder> rec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recorder_) return "";
        engine_.setRecorder(nullptr);
        rec = std::move(recorder_);
    }
    engine_.waitForRender();
    rec->stop();
    log::info("recording stopped: %s, %.1f s%s", rec->path().c_str(), rec->seconds(),
              rec->droppedFrames() ? " (some audio lost: the disk was too slow)" : "");
    return rec->path();
}

Recording::Status Recording::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    Status s;
    if (!recorder_) return s;
    s.recording = true;
    s.path = recorder_->path();
    s.seconds = recorder_->seconds();
    s.dropped = recorder_->droppedFrames();
    s.failed = recorder_->failed();
    return s;
}

} // namespace mbx
