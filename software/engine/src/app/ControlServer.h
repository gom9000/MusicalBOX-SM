#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace mbx {

// Local control socket (Unix domain, stream): one JSON request per line, one
// JSON reply per line. Used by mbx-control (web editor, later the panel).
// A single thread serves every client; the handler runs on that thread.
class ControlServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;

    ~ControlServer();
    // False (error set) if the socket cannot be created.
    bool start(const std::string& path, Handler handler, std::string& error);
    void stop();

private:
    void run();

    std::string path_;
    Handler handler_;
    int listenFd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace mbx
