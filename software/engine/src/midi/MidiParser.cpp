#include "midi/MidiParser.h"

namespace mbx {

void MidiParser::reset() {
    runningStatus_ = 0;
    count_ = 0;
    inSysEx_ = false;
}

bool MidiParser::feed(std::uint8_t byte, MidiEvent& out) {
    if (byte >= 0xF8) {
        // Real-time (clock, start, stop, active sensing...): ignored, and it
        // must not disturb a message in progress.
        return false;
    }

    if (byte & 0x80) {
        // Status byte.
        if (byte == 0xF0) {
            inSysEx_ = true;
            runningStatus_ = 0;
            count_ = 0;
            return false;
        }
        if (byte >= 0xF0) {
            // End of SysEx or system common: cancels running status.
            inSysEx_ = false;
            runningStatus_ = 0;
            count_ = 0;
            return false;
        }
        inSysEx_ = false;
        runningStatus_ = byte;
        count_ = 0;
        return false;
    }

    // Data byte.
    if (inSysEx_ || runningStatus_ == 0) return false;

    data_[count_++] = byte;
    if (count_ < midi::dataLength(runningStatus_)) return false;

    out.status = runningStatus_;
    out.data1 = data_[0];
    out.data2 = (count_ > 1) ? data_[1] : 0;
    count_ = 0;  // keep running status for the next message
    return true;
}

} // namespace mbx
