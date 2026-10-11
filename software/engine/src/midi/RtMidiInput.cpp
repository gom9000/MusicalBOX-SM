#include "midi/RtMidiInput.h"

#include "core/Engine.h"
#include "core/Log.h"
#include "midi/MidiEvent.h"

#include "rtmidi/RtMidi.h"

#include <algorithm>
#include <cstdio>

namespace mbx {

struct RtMidiInput::Port {
    std::unique_ptr<RtMidiIn> in;
    std::string name;
    int source = -1;
    Engine* engine = nullptr;
};

namespace {

bool ignored(const std::string& name) {
    return name.find("Midi Through") != std::string::npos || name.find("RtMidi") != std::string::npos;
}

} // namespace

RtMidiInput::RtMidiInput(Engine& engine) : engine_(engine) {}

RtMidiInput::~RtMidiInput() {
    close();
}

void RtMidiInput::onMessage(double, std::vector<unsigned char>* message, void* user) {
    auto* port = static_cast<Port*>(user);
    if (!message || message->empty()) return;
    const unsigned char status = (*message)[0];
    if (status < 0x80 || status >= 0xF0) return;  // only channel messages
    MidiEvent event;
    event.status = status;
    event.data1 = message->size() > 1 ? (*message)[1] : 0;
    event.data2 = message->size() > 2 ? (*message)[2] : 0;
    port->engine->post(port->source, EngineEvent::fromMidi(event));
}

void RtMidiInput::scan() {
    std::vector<std::string> available;
    try {
        RtMidiIn probe(RtMidi::UNSPECIFIED, "mbx-engine probe");
        const unsigned int count = probe.getPortCount();
        for (unsigned int i = 0; i < count; ++i) available.push_back(probe.getPortName(i));
    } catch (const RtMidiError& e) {
        if (systemAvailable_) log::warn("MIDI system not available: %s", e.getMessage().c_str());
        systemAvailable_ = false;
        return;
    }
    if (!systemAvailable_) log::info("MIDI system available again");
    systemAvailable_ = true;

    // Close ports that disappeared.
    for (auto it = ports_.begin(); it != ports_.end();) {
        if (std::find(available.begin(), available.end(), (*it)->name) == available.end()) {
            log::info("MIDI input closed: %s", (*it)->name.c_str());
            (*it)->in.reset();  // stops RtMidi's thread before the source is reused
            engine_.releaseSource((*it)->source);
            it = ports_.erase(it);
        } else {
            ++it;
        }
    }

    // Open new ones.
    for (unsigned int i = 0; i < available.size(); ++i) {
        const std::string& name = available[i];
        if (ignored(name)) continue;
        const bool open = std::any_of(ports_.begin(), ports_.end(), [&](const auto& p) { return p->name == name; });
        if (open) continue;

        auto port = std::make_unique<Port>();
        port->name = name;
        port->engine = &engine_;
        port->source = engine_.registerSource();
        if (port->source < 0) {
            log::warn("MIDI input %s not opened: too many sources", name.c_str());
            continue;
        }
        try {
            port->in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "mbx-engine");
            port->in->ignoreTypes(true, true, true);  // no SysEx, timing, active sensing
            port->in->setCallback(&RtMidiInput::onMessage, port.get());
            port->in->openPort(i, "in");
        } catch (const RtMidiError& e) {
            log::warn("MIDI input %s not opened: %s", name.c_str(), e.getMessage().c_str());
            engine_.releaseSource(port->source);
            continue;
        }
        log::info("MIDI input opened: %s", name.c_str());
        ports_.push_back(std::move(port));
    }
}

void RtMidiInput::close() {
    for (auto& port : ports_) {
        port->in.reset();
        engine_.releaseSource(port->source);
    }
    ports_.clear();
}

void RtMidiInput::listPorts() {
    try {
        RtMidiIn probe(RtMidi::UNSPECIFIED, "mbx-engine probe");
        const unsigned int count = probe.getPortCount();
        std::printf("MIDI inputs:\n");
        for (unsigned int i = 0; i < count; ++i) {
            const std::string name = probe.getPortName(i);
            std::printf("  %s%s\n", name.c_str(), ignored(name) ? "  [ignored]" : "");
        }
        if (count == 0) std::printf("  (none)\n");
    } catch (const RtMidiError& e) {
        std::printf("MIDI unavailable: %s\n", e.getMessage().c_str());
    }
}

} // namespace mbx
