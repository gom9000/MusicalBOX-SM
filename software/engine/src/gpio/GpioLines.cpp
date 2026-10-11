#include "gpio/GpioLines.h"

#if defined(__linux__)

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mbx {

namespace {

// The SoC's GPIO controller of a Raspberry Pi (bcm2835/bcm2711, rp1 on the Pi 5).
bool isPiChip(const char* label) {
    return std::strncmp(label, "pinctrl-bcm", 11) == 0 || std::strncmp(label, "pinctrl-rp1", 11) == 0;
}

std::string findPiChip() {
    for (int i = 0; i < 16; ++i) {
        const std::string path = "/dev/gpiochip" + std::to_string(i);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        gpiochip_info info{};
        const bool pi = ::ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && isPiChip(info.label);
        ::close(fd);
        if (pi) return path;
    }
    return {};
}

} // namespace

GpioLines::~GpioLines() { close(); }

GpioLines::Result GpioLines::open(const std::string& chip, const std::vector<unsigned>& offsets,
                                  const std::string& consumer, std::string& message) {
    close();
    if (offsets.empty() || offsets.size() > GPIO_V2_LINES_MAX) {
        message = "no GPIO lines asked";
        return Result::Failed;
    }
    std::string path = chip.empty() ? findPiChip() : chip.rfind("/dev/", 0) == 0 ? chip : "/dev/" + chip;
    if (path.empty()) {
        message = "no Raspberry Pi GPIO chip";
        return Result::NoChip;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        message = path + " not found";
        return chip.empty() ? Result::NoChip : Result::Failed;
    }
    const int chipFd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (chipFd < 0) {
        message = path + ": " + std::strerror(errno) + (errno == EACCES ? " (the user must be in the gpio group)" : "");
        return Result::Failed;
    }
    gpio_v2_line_request req{};
    for (std::size_t i = 0; i < offsets.size(); ++i) req.offsets[i] = offsets[i];
    req.num_lines = static_cast<__u32>(offsets.size());
    std::snprintf(req.consumer, sizeof req.consumer, "%s", consumer.c_str());
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;  // start low
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0;
    req.config.attrs[0].mask = (offsets.size() >= 64 ? ~0ull : (1ull << offsets.size()) - 1);
    const int rc = ::ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req);
    const int err = errno;
    ::close(chipFd);
    if (rc < 0) {
        message = path + ": " + std::strerror(err) + (err == EBUSY ? " (a line is used by something else)" : "");
        return Result::Failed;
    }
    fd_ = req.fd;
    count_ = req.num_lines;
    chipPath_ = path;
    return Result::Ok;
}

void GpioLines::set(unsigned mask) {
    if (fd_ < 0) return;
    gpio_v2_line_values v{};
    v.mask = (1ull << count_) - 1;
    v.bits = mask & v.mask;
    ::ioctl(fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

void GpioLines::close() {
    if (fd_ < 0) return;
    set(0);  // released lines keep their last level: leave them low (muted)
    ::close(fd_);
    fd_ = -1;
}

} // namespace mbx

#else  // no GPIO character device

namespace mbx {

GpioLines::~GpioLines() = default;

GpioLines::Result GpioLines::open(const std::string&, const std::vector<unsigned>&, const std::string&,
                                  std::string& message) {
    message = "no GPIO on this system";
    return Result::NoChip;
}

void GpioLines::set(unsigned) {}
void GpioLines::close() {}

} // namespace mbx

#endif
