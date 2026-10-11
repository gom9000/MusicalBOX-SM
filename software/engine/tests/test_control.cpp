// Live editing: a new preset table replaces the old one while notes play
// (same program: notes go on, effects cross-fade), old tables are freed only
// after the engine stopped using them, and the control socket commands.

#include "check.h"
#include "app/BankManager.h"
#include "app/ControlApi.h"
#include "app/ControlServer.h"
#include "app/Recording.h"
#include "dr_libs/dr_wav.h"
#include "core/Engine.h"
#include "sampler/Instrument.h"
#include "sampler/InstrumentStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace mbx;
using nlohmann::json;

namespace {

constexpr double kRate = 48000.0;

std::unique_ptr<Instrument> sine(const std::string& id, double freq) {
    Zone z;
    z.channels = 1;
    z.sampleRate = kRate;
    z.frames = static_cast<std::uint32_t>(2 * kRate);
    z.rootNote = 60;
    z.data.resize(z.frames);
    for (std::uint32_t i = 0; i < z.frames; ++i)
        z.data[i] = static_cast<std::int16_t>(std::lround(0.4 * 32767.0 * std::sin(2 * 3.14159265 * freq * i / kRate)));
    auto inst = std::make_unique<Instrument>(id, id);
    inst->addZone(std::move(z));
    inst->finalize(FillMode::Nearest);
    return inst;
}

json programs(double toneGainDb, const char* secondId = "b") {
    return json::parse(R"({"version": 1, "programs": [
        {"id": "a", "name": "A", "layers": [{"instrument": "SINE"}],
         "fx": [{"type": "tone", "params": {"gain_db": )" + std::to_string(toneGainDb) + R"(}}]},
        {"id": ")" + std::string(secondId) + R"(", "name": "B", "layers": [{"instrument": "SINE"}]}
    ]})");
}

json presets(const char* first = "a", const char* second = "b") {
    return json{{"version", 1}, {"startup", 1}, {"presets", {{"1", first}, {"2", second}}}};
}

// RMS of the left channel over `seconds`.
double render(Engine& engine, double seconds) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> st(2 * 128);
    double sum = 0;
    for (int pos = 0; pos < frames; pos += 128) {
        engine.render(st.data(), 128);
        for (int i = 0; i < 128; ++i) sum += double(st[2 * i]) * st[2 * i];
    }
    return std::sqrt(sum / frames);
}

json call(ControlApi& api, const json& req) {
    return json::parse(api.handle(req.dump()));
}

} // namespace

int main() {
    InstrumentStore store;
    store.add(sine("SINE", 250.0));

    Engine engine;
    engine.setMasterGainDb(0.0f);
    BankManager banks(engine, store, 3);
    std::vector<std::string> warnings;
    std::string error;
    auto first = banks.build(programs(0.0), presets(), warnings, error);
    CHECK(first != nullptr);
    if (!first) return finish("test_control");
    std::weak_ptr<ProgramBank> firstAlive = first;
    banks.install(first);
    first.reset();
    engine.prepare(kRate);
    CHECK(engine.selectPresetNow(1));
    const int src = engine.registerSource();

    // --- Same program, new effects: the held note goes on, the level follows the new chain.
    engine.post(src, EngineEvent::fromMidi({0x90, 60, 127}));
    const double before = render(engine, 0.2);
    auto quieter = banks.build(programs(-12.0), presets(), warnings, error);
    CHECK(quieter && banks.swap(quieter, src));
    const double during = render(engine, 0.01);
    render(engine, 0.05);  // past the cross-fade
    const double after = render(engine, 0.2);
    CHECK(engine.activeVoices() == 1);                  // not cut
    CHECK(during > 0.2 * before);                       // no gap during the cross-fade
    CHECK_NEAR(20 * std::log10(after / before), -12.0, 0.5);
    CHECK(engine.currentSlot() == 1);
    CHECK(engine.appliedGeneration() == 1);

    // --- The old bank lives until the engine applied the new one, then a little longer.
    banks.collect();
    CHECK(!firstAlive.expired());  // applied, but the grace time is not over
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    banks.collect();
    CHECK(firstAlive.expired());

    // --- The program playing is gone: the button's new program, notes cut.
    auto renamed = banks.build(json::parse(R"({"version": 1, "programs": [
        {"id": "c", "name": "C", "layers": [{"instrument": "SINE"}]},
        {"id": "b", "name": "B", "layers": [{"instrument": "SINE"}]}]})"),
                               presets("c", "b"), warnings, error);
    CHECK(renamed && banks.swap(renamed, src));
    render(engine, 0.05);
    CHECK(engine.activeVoices() == 0);
    CHECK(engine.currentSlot() == 1);

    // --- Unusable bank: refused with the reason.
    warnings.clear();
    CHECK(banks.build(json::parse(R"({"version": 1, "programs": []})"), presets(), warnings, error) == nullptr);

    // --- Control API -----------------------------------------------------------
    LiveStats stats;
    ControlApi api(engine, banks, store, "does-not-exist", stats, {"startup warning"}, 1, "test");
    json r = call(api, {{"cmd", "status"}});
    CHECK(r["ok"] == true && r["slot"] == 1 && r["program"]["id"] == "c" && r["version"] == "test");
    r = call(api, {{"cmd", "bank"}});
    CHECK(r["ok"] == true && r["presets"].size() == kPresetCount && r["presets"][1] == "b" && r["presets"][2].is_null());
    r = call(api, {{"cmd", "instruments"}});
    CHECK(r["instruments"].size() == 1 && r["instruments"][0]["id"] == "SINE");
    r = call(api, {{"cmd", "preset"}, {"slot", 2}});
    CHECK(r["ok"] == true);
    render(engine, 0.01);
    CHECK(engine.currentSlot() == 2);
    CHECK(call(api, {{"cmd", "preset"}, {"slot", 7}})["ok"] == false);  // empty
    CHECK(call(api, {{"cmd", "preset"}, {"slot", 129}})["ok"] == false);
    CHECK(call(api, {{"cmd", "preset"}, {"slot", 100}})["ok"] == false);  // not defined
    r = call(api, {{"cmd", "note"}, {"key", 60}, {"velocity", 100}});
    render(engine, 0.01);
    CHECK(r["ok"] == true && engine.activeVoices() == 1);
    call(api, {{"cmd", "note"}, {"key", 60}, {"velocity", 0}});
    CHECK(call(api, {{"cmd", "volume"}, {"db", -3}})["ok"] == true);
    render(engine, 0.01);
    CHECK_NEAR(engine.masterGainDb(), -3.0, 1e-6);

    r = call(api, {{"cmd", "preview"}, {"programs", programs(-6.0, "b")}, {"presets", presets()}});
    CHECK(r["ok"] == true);
    render(engine, 0.01);
    CHECK(call(api, {{"cmd", "status"}})["edited"] == true);
    r = call(api, {{"cmd", "check"}, {"programs", json::parse(R"({"version":1,"programs":[]})")}, {"presets", presets()}});
    CHECK(r["ok"] == false);
    r = call(api, {{"cmd", "reload"}});  // config folder missing: error, nothing changes
    CHECK(r["ok"] == false);
    CHECK(call(api, {{"cmd", "warnings"}})["startup"][0] == "startup warning");
    CHECK(call(api, {{"cmd", "warnings"}})["active"].size() == 1);  // a preview does not change them
    CHECK(json::parse(api.handle("not json"))["ok"] == false);
    CHECK(call(api, {{"cmd", "nope"}})["ok"] == false);

    // --- Recording: what the engine plays ends up in a stereo WAV, through the API too.
    {
        Recording recording(engine, "/tmp");
        api.setRecording(&recording);
        r = call(api, {{"cmd", "record"}, {"action", "start"}, {"path", "/tmp/mbx-test-rec.wav"}});
        CHECK(r["ok"] == true && r["recording"] == true);
        CHECK(call(api, {{"cmd", "status"}})["recording"] == true);
        CHECK(call(api, {{"cmd", "record"}, {"action", "start"}})["ok"] == false);  // already recording
        call(api, {{"cmd", "note"}, {"key", 60}, {"velocity", 100}});
        render(engine, 0.5);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the writer catches up
        r = call(api, {{"cmd", "record"}, {"action", "stop"}});
        CHECK(r["ok"] == true && r["recording"] == false && r["path"] == "/tmp/mbx-test-rec.wav");
        call(api, {{"cmd", "note"}, {"key", 60}, {"velocity", 0}});
        unsigned ch = 0, rate = 0;
        drwav_uint64 frames = 0;
        float* data = drwav_open_file_and_read_pcm_frames_f32("/tmp/mbx-test-rec.wav", &ch, &rate, &frames, nullptr);
        CHECK(data != nullptr && ch == 2 && rate == 48000);
        CHECK(frames >= 23000 && frames <= 24400);  // 0.5 s, in whole blocks
        float peak = 0;
        for (drwav_uint64 i = 0; data && i < frames * 2; ++i) peak = std::max(peak, std::fabs(data[i]));
        CHECK(peak > 0.05f);
        drwav_free(data, nullptr);
        std::remove("/tmp/mbx-test-rec.wav");
        api.setRecording(nullptr);
    }

#ifndef _WIN32
    // --- Socket: one request per line, one reply per line.
    {
        ControlServer server;
        const std::string path = "/tmp/mbx-test-control.sock";
        CHECK(server.start(path, [&](const std::string& req) { return api.handle(req); }, error));
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        const std::string req = "{\"cmd\":\"status\"}\n{\"cmd\":\"bank\"}\n";
        CHECK(::write(fd, req.data(), req.size()) == static_cast<ssize_t>(req.size()));
        std::string got;
        char buf[4096];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::count(got.begin(), got.end(), '\n') < 2 && std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0) break;
            got.append(buf, static_cast<std::size_t>(n));
        }
        ::close(fd);
        const auto nl = got.find('\n');
        CHECK(nl != std::string::npos);
        if (nl != std::string::npos) {
            CHECK(json::parse(got.substr(0, nl))["ok"] == true);
            CHECK(json::parse(got.substr(nl + 1))["presets"].size() == kPresetCount);
        }
        server.stop();
        CHECK(::access(path.c_str(), F_OK) != 0);  // removed
    }
#endif

    return finish("test_control");
}
