#pragma once

#include <string>
#include <vector>

namespace mbx {

// A few GPIO lines as outputs, through the Linux GPIO character device
// (/dev/gpiochipN, uAPI v2: no library needed). Requested low. Elsewhere
// (Windows, a PC without the Pi's GPIO chip) open() says "not available".
class GpioLines {
public:
    GpioLines() = default;
    ~GpioLines();
    GpioLines(const GpioLines&) = delete;
    GpioLines& operator=(const GpioLines&) = delete;

    enum class Result { Ok, NoChip, Failed };
    // chip: "gpiochip0", "/dev/gpiochip0" or "" = the Raspberry Pi's own GPIO chip.
    Result open(const std::string& chip, const std::vector<unsigned>& offsets, const std::string& consumer,
                std::string& message);
    void set(unsigned mask);  // bit i = offsets[i] high
    void close();
    bool isOpen() const { return fd_ >= 0; }
    const std::string& chipPath() const { return chipPath_; }

private:
    int fd_ = -1;
    unsigned count_ = 0;
    std::string chipPath_;
};

} // namespace mbx
