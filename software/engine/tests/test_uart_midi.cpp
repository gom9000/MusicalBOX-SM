// DIN MIDI input on a pseudo-terminal standing in for the UART: bytes as a
// keyboard sends them (active sensing, running status, note on with
// velocity 0) must play and release notes; a vanished port is reopened.

#include "check.h"
#include "core/Engine.h"
#include "midi/UartMidiInput.h"
#include "sampler/Instrument.h"
#include "sampler/Program.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

using namespace mbx;

namespace {

constexpr double kRate = 48000.0;

Zone makeTone() {
    Zone z;
    z.channels = 1;
    z.sampleRate = kRate;
    z.frames = static_cast<std::uint32_t>(kRate);
    z.rootNote = 60;
    z.data.resize(z.frames);
    for (std::uint32_t i = 0; i < z.frames; ++i)
        z.data[i] = static_cast<std::int16_t>(std::lround(8000.0 * std::sin(2 * 3.14159265 * 250.0 * i / kRate)));
    return z;
}

// Opens a pty pair; returns the master fd, fills the slave path.
int openPty(std::string& slave) {
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) return -1;
    slave = ptsname(master);
    return master;
}

void send(int fd, std::vector<unsigned char> bytes) {
    CHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
}

// Lets the reader thread pick up the bytes, then renders so the engine
// applies the queued events. Returns the voices sounding afterwards.
int settle(Engine& engine) {
    std::vector<float> buffer(2 * 128);
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        engine.render(buffer.data(), 128);
    }
    return engine.activeVoices();
}

} // namespace

int main() {
    Instrument tone("TONE", "tone");
    tone.addZone(makeTone());
    tone.finalize(FillMode::Nearest);
    Program program;
    Layer layer;
    layer.instrument = &tone;
    layer.adsr.releaseMs = 1.0f;
    program.layers.push_back(layer);

    Engine engine;
    engine.prepare(kRate);
    engine.setProgram(&program);

    std::string slave;
    int master = openPty(slave);
    CHECK(master >= 0);
    if (master < 0) return finish("test_uart_midi");

    UartMidiInput din(engine, slave);
    din.scan();
    CHECK(din.isOpen());

    // Active sensing around a chord in running status: 3 notes.
    send(master, {0xFE, 0xFE, 0x90, 0x39, 0x63, 0xFE, 0x3B, 0x63, 0x3C, 0x63, 0xFE});
    CHECK(settle(engine) == 3);
    // Released with note on, velocity 0 (running status still 0x90).
    send(master, {0x39, 0x00, 0x3B, 0x00, 0xFE, 0x3C, 0x00});
    CHECK(settle(engine) == 0);
    CHECK(din.bytesReceived() == 18);

    // A real-time byte in the middle of a message must not break it.
    send(master, {0x90, 0x40, 0xF8, 0x63});
    CHECK(settle(engine) == 1);
    send(master, {0x80, 0x40, 0x00});
    CHECK(settle(engine) == 0);

    // The port goes away: the reader stops, the next scan reopens it.
    ::close(master);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    master = openPty(slave);  // new pty, likely a different path: reopen the same object on it
    UartMidiInput din2(engine, slave);
    din2.scan();
    CHECK(din2.isOpen());
    send(master, {0x90, 0x30, 0x40});
    CHECK(settle(engine) == 1);
    din.scan();  // the first one notices its port failed and tries again (no crash, no hang)
    din.close();
    din2.close();
    ::close(master);

    // A missing device is not an error, just "not available".
    UartMidiInput missing(engine, "/dev/does-not-exist");
    missing.scan();
    CHECK(!missing.isOpen());
    UartMidiInput disabled(engine, "");
    disabled.scan();
    CHECK(!disabled.enabled());

    return finish("test_uart_midi");
}

#else
int main() { return 0; }
#endif
