#include "check.h"
#include "midi/MidiParser.h"

#include <vector>

using namespace mbx;

static std::vector<MidiEvent> parse(const std::vector<std::uint8_t>& bytes) {
    MidiParser parser;
    std::vector<MidiEvent> out;
    MidiEvent e;
    for (auto b : bytes)
        if (parser.feed(b, e)) out.push_back(e);
    return out;
}

int main() {
    // Plain note on / note off.
    auto ev = parse({0x90, 60, 100, 0x80, 60, 0});
    CHECK(ev.size() == 2);
    CHECK(ev[0].command() == midi::kNoteOn && ev[0].data1 == 60 && ev[0].data2 == 100);
    CHECK(ev[1].command() == midi::kNoteOff && ev[1].data1 == 60);

    // Running status: one status byte, three notes.
    ev = parse({0x91, 60, 100, 64, 90, 67, 0});
    CHECK(ev.size() == 3);
    CHECK(ev[2].status == 0x91 && ev[2].data1 == 67 && ev[2].data2 == 0);
    CHECK(ev[0].channel() == 2);

    // Real-time bytes in the middle of a message are ignored.
    ev = parse({0x90, 0xF8, 60, 0xFE, 100});
    CHECK(ev.size() == 1 && ev[0].data1 == 60 && ev[0].data2 == 100);

    // Program change has a single data byte.
    ev = parse({0xC0, 5, 6});
    CHECK(ev.size() == 2 && ev[0].data1 == 5 && ev[1].data1 == 6);

    // SysEx is skipped and cancels running status.
    ev = parse({0x90, 60, 100, 0xF0, 1, 2, 3, 0xF7, 61, 100, 0x90, 62, 100});
    CHECK(ev.size() == 2 && ev[1].data1 == 62);

    // Data before any status byte is ignored.
    ev = parse({60, 100, 0xB0, 64, 127});
    CHECK(ev.size() == 1 && ev[0].command() == midi::kControlChange && ev[0].data1 == 64);

    return finish("test_midi_parser");
}
