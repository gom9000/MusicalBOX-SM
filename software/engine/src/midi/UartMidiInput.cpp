#include "midi/UartMidiInput.h"

#include "core/Engine.h"
#include "core/Log.h"
#include "midi/MidiParser.h"

#ifndef _WIN32

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace mbx {

namespace {

constexpr int kReaderPriority = 70;  // below the audio thread, above everything else
constexpr int kPollMs = 100;         // how often the reader checks for close()

std::string realPath(const std::string& device) {
    std::error_code ec;
    const auto target = std::filesystem::canonical(device, ec);
    return ec ? device : target.string();
}

} // namespace

UartMidiInput::UartMidiInput(Engine& engine, std::string device) : engine_(engine), device_(std::move(device)) {}

UartMidiInput::~UartMidiInput() {
    close();
}

void UartMidiInput::scan() {
    if (device_.empty()) return;
    if (fd_ >= 0 && !failed_.load()) return;  // already running
    if (fd_ >= 0) {
        close();
        log::warn("MIDI serial %s: read error, port closed - retrying", device_.c_str());
        lastProblem_.clear();
    }
    open();
}

bool UartMidiInput::open() {
    auto problem = [&](const std::string& what) {
        if (what != lastProblem_) log::warn("MIDI serial %s: %s", device_.c_str(), what.c_str());
        lastProblem_ = what;
        return false;
    };

    std::error_code ec;
    if (!std::filesystem::exists(device_, ec))
        return problem("not found (enable_uart=1 in config.txt?) - DIN MIDI not available");

    const int fd = ::open(device_.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        if (err == EACCES)
            return problem("permission denied - add the user to the dialout group (sudo usermod -aG dialout $USER)");
        if (err == EBUSY) return problem("busy - is a login console (serial-getty) still using it?");
        return problem(std::strerror(err));
    }
    if (::ioctl(fd, TIOCEXCL) != 0) {
        // Not fatal: only keeps other programs from opening the port while we use it.
    }

    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        const int err = errno;
        ::close(fd);
        return problem(std::string("not a serial port: ") + std::strerror(err));
    }
    ::cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    ::cfsetispeed(&tio, B38400);
    ::cfsetospeed(&tio, B38400);
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        const int err = errno;
        ::close(fd);
        return problem(std::string("cannot set 38400 baud: ") + std::strerror(err));
    }
    ::tcflush(fd, TCIFLUSH);

    const int source = engine_.registerSource();
    if (source < 0) {
        ::close(fd);
        return problem("not opened: too many MIDI sources");
    }

    fd_ = fd;
    source_ = source;
    stop_ = false;
    failed_ = false;
    thread_ = std::thread(&UartMidiInput::run, this);
    lastProblem_.clear();
    log::info("MIDI input opened: DIN on %s (%s, 31250 baud with dtoverlay=midi-uart0)", device_.c_str(),
              realPath(device_).c_str());
    return true;
}

void UartMidiInput::run() {
    sched_param sp{};
    sp.sched_priority = kReaderPriority;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);  // best effort: without rtprio it stays normal

    MidiParser parser;
    MidiEvent event;
    bool announced = false;
    unsigned char buffer[64];

    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, kPollMs);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        const ssize_t n = ::read(fd_, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;

        bytes_.fetch_add(static_cast<unsigned long>(n), std::memory_order_relaxed);
        for (ssize_t i = 0; i < n; ++i) {
            if (parser.feed(buffer[i], event)) {
                if (!announced) {
                    log::info("MIDI serial: receiving");
                    announced = true;
                }
                engine_.post(source_, EngineEvent::fromMidi(event));
            }
        }
    }
    if (!stop_.load()) failed_ = true;
}

void UartMidiInput::close() {
    if (fd_ < 0) return;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    ::close(fd_);
    fd_ = -1;
    engine_.releaseSource(source_);
    source_ = -1;
}

void UartMidiInput::describe(const std::string& device) {
    if (device.empty()) {
        std::printf("MIDI serial (DIN): disabled in engine.json\n");
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(device, ec)) {
        std::printf("MIDI serial (DIN): %s not found\n", device.c_str());
        return;
    }
    const bool readable = ::access(device.c_str(), R_OK) == 0;
    std::printf("MIDI serial (DIN): %s -> %s%s\n", device.c_str(), realPath(device).c_str(),
                readable ? "" : "  [no permission: user not in group dialout]");
}

} // namespace mbx

#else  // no UART on Windows: DIN MIDI is a Raspberry Pi feature

namespace mbx {
UartMidiInput::UartMidiInput(Engine& engine, std::string device) : engine_(engine), device_(std::move(device)) {}
UartMidiInput::~UartMidiInput() = default;
void UartMidiInput::scan() {}
void UartMidiInput::close() {}
bool UartMidiInput::open() { return false; }
void UartMidiInput::run() {}
void UartMidiInput::describe(const std::string&) {}
} // namespace mbx

#endif
