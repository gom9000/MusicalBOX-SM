#include "app/ControlServer.h"

#include "core/Log.h"

#ifndef _WIN32

#include <cerrno>
#include <cstring>
#include <map>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace mbx {

namespace {
constexpr int kMaxClients = 8;
constexpr std::size_t kMaxRequest = 4 * 1024 * 1024;  // a whole programs.json fits
}

ControlServer::~ControlServer() {
    stop();
}

bool ControlServer::start(const std::string& path, Handler handler, std::string& error) {
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) {
        error = "control socket path too long: " + path;
        return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("control socket: ") + std::strerror(errno);
        return false;
    }
    ::unlink(path.c_str());  // left over by a previous run
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 4) != 0) {
        error = "control socket " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::chmod(path.c_str(), 0660);
    path_ = path;
    handler_ = std::move(handler);
    listenFd_ = fd;
    stop_ = false;
    thread_ = std::thread(&ControlServer::run, this);
    return true;
}

void ControlServer::stop() {
    if (listenFd_ < 0) return;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    ::close(listenFd_);
    listenFd_ = -1;
    ::unlink(path_.c_str());
}

void ControlServer::run() {
    std::map<int, std::string> clients;  // fd -> bytes received, not yet a full line
    auto drop = [&](int fd) {
        ::close(fd);
        clients.erase(fd);
    };
    while (!stop_.load()) {
        std::vector<pollfd> fds;
        fds.push_back({listenFd_, POLLIN, 0});
        for (const auto& c : clients) fds.push_back({c.first, POLLIN, 0});
        const int r = ::poll(fds.data(), fds.size(), 200);
        if (r <= 0) continue;

        if (fds[0].revents & POLLIN) {
            const int c = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c >= 0) {
                if (static_cast<int>(clients.size()) >= kMaxClients)
                    ::close(c);
                else
                    clients[c] = std::string();
            }
        }
        for (std::size_t i = 1; i < fds.size(); ++i) {
            const int fd = fds[i].fd;
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[65536];
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0) {
                drop(fd);
                continue;
            }
            std::string& pending = clients[fd];
            pending.append(buf, static_cast<std::size_t>(n));
            if (pending.size() > kMaxRequest) {
                drop(fd);
                continue;
            }
            std::size_t nl;
            bool ok = true;
            while (ok && (nl = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (line.empty()) continue;
                std::string reply = handler_(line) + "\n";
                // Replies are small; the client reads them at once. Blocking write is fine.
                std::size_t sent = 0;
                while (sent < reply.size()) {
                    const ssize_t w = ::send(fd, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
                    if (w <= 0) {
                        ok = false;
                        break;
                    }
                    sent += static_cast<std::size_t>(w);
                }
            }
            if (!ok) drop(fd);
        }
    }
    for (const auto& c : clients) ::close(c.first);
}

} // namespace mbx

#else

namespace mbx {
ControlServer::~ControlServer() = default;
bool ControlServer::start(const std::string&, Handler, std::string& error) {
    error = "control socket not available on Windows";
    return false;
}
void ControlServer::stop() {}
void ControlServer::run() {}
} // namespace mbx

#endif
