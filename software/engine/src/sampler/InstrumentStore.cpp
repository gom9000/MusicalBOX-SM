#include "sampler/InstrumentStore.h"

#include "sampler/NoteName.h"
#include "sampler/WavLoader.h"

#include <algorithm>
#include <thread>
#include <atomic>
#include <cctype>
#include <map>
#include <regex>
#include <set>

namespace mbx {
namespace {

enum class Field { Note, Name, Vel };

struct FilePattern {
    std::regex regex;
    std::vector<Field> fields;  // capture groups, in order
};

// Text labels of {vel} ("PP", "FF"...), longest first so "FF" is tried before "F".
std::string labelAlternation(std::vector<std::string> labels) {
    std::sort(labels.begin(), labels.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    std::string rx = "(";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i) rx += '|';
        for (char c : labels[i]) {
            if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) rx += '\\';
            rx += c;
        }
    }
    return rx + ")";
}

// "{name}_{vel}.wav" -> ^([A-Ga-g][#b]?-?\d{1,2})_(\d{1,3})\.wav$ (case-insensitive)
bool compilePattern(const std::string& pattern, const std::vector<std::string>& velLabels, FilePattern& out,
                    std::string& error) {
    std::string rx = "^";
    int noteFields = 0;
    int velFields = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '{') {
            const std::size_t close = pattern.find('}', i);
            if (close == std::string::npos) {
                error = "unclosed '{' in \"" + pattern + "\"";
                return false;
            }
            const std::string token = pattern.substr(i + 1, close - i - 1);
            if (token == "note") {
                rx += "(\\d{1,3})";
                out.fields.push_back(Field::Note);
                ++noteFields;
            } else if (token == "name") {
                rx += "([A-Ga-g][#b]?-?\\d{1,2})";
                out.fields.push_back(Field::Name);
                ++noteFields;
            } else if (token == "vel") {
                rx += velLabels.empty() ? std::string("(\\d{1,3})") : labelAlternation(velLabels);
                out.fields.push_back(Field::Vel);
                ++velFields;
            } else {
                error = "unknown field {" + token + "} in \"" + pattern + "\"";
                return false;
            }
            i = close;
        } else if (c == '*') {
            rx += ".*?";
        } else {
            if (std::string("\\^$.|?+()[]{}").find(c) != std::string::npos) rx += '\\';
            rx += c;
        }
    }
    rx += "$";
    if (noteFields != 1) {
        error = "\"" + pattern + "\" must contain exactly one {note} or {name}";
        return false;
    }
    if (velFields > 1) {
        error = "\"" + pattern + "\" contains {vel} more than once";
        return false;
    }
    out.regex = std::regex(rx, std::regex::icase);
    return true;
}

struct FileMatch {
    std::filesystem::path path;
    int note = -1;
    int velIndex = 0;  // 0 = no {vel} in the pattern
};

} // namespace

std::unique_ptr<Instrument> InstrumentStore::load(const InstrumentDef& def, const std::filesystem::path& mediaRoot,
                                                  std::vector<std::string>& warnings) {
    const std::string tag = "instrument " + def.id + ": ";
    const std::filesystem::path dir = mediaRoot / def.path;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        warnings.push_back(tag + "folder " + dir.string() + " not found - skipped");
        return nullptr;
    }

    FilePattern pattern;
    std::string error;
    std::vector<std::string> velLabels;
    for (const auto& label : def.velocityLabels) velLabels.push_back(label.first);
    if (!compilePattern(def.files, velLabels, pattern, error)) {
        warnings.push_back(tag + error + " - skipped");
        return nullptr;
    }

    // Collect matching files, sorted by name for a stable result.
    std::vector<FileMatch> matches;
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_regular_file()) entries.push_back(entry.path());
    std::sort(entries.begin(), entries.end());

    for (const auto& path : entries) {
        const std::string filename = path.filename().string();
        std::smatch m;
        if (!std::regex_match(filename, m, pattern.regex)) continue;
        FileMatch match;
        match.path = path;
        for (std::size_t g = 0; g < pattern.fields.size(); ++g) {
            const std::string text = m[g + 1].str();
            switch (pattern.fields[g]) {
            case Field::Note: match.note = std::stoi(text); break;
            case Field::Name: {
                const int n = parseNoteName(text);
                match.note = n < 0 ? -1 : n + def.nameOffset;
                break;
            }
            case Field::Vel:
                if (velLabels.empty()) {
                    match.velIndex = std::stoi(text);
                } else {
                    // Index 1..N in velocity order, labels compared ignoring case.
                    auto same = [&](const std::string& a) {
                        return a.size() == text.size() &&
                               std::equal(a.begin(), a.end(), text.begin(), [](char x, char y) {
                                   return std::tolower(static_cast<unsigned char>(x)) ==
                                          std::tolower(static_cast<unsigned char>(y));
                               });
                    };
                    for (std::size_t i = 0; i < velLabels.size(); ++i)
                        if (same(velLabels[i])) match.velIndex = static_cast<int>(i) + 1;
                }
                break;
            }
        }
        if (match.note < 0 || match.note > 127) {
            warnings.push_back(tag + filename + ": note out of range - ignored");
            continue;
        }
        matches.push_back(match);
    }
    if (matches.empty()) {
        warnings.push_back(tag + "no file in " + dir.string() + " matches \"" + def.files + "\" - skipped");
        return nullptr;
    }

    // Velocity index -> velocity range.
    const bool hasVel = std::find(pattern.fields.begin(), pattern.fields.end(), Field::Vel) != pattern.fields.end();
    std::map<int, std::pair<int, int>> velRanges;
    if (hasVel) {
        if (!def.velocityLayers.empty()) {
            const auto& layers = def.velocityLayers;
            for (std::size_t i = 0; i < layers.size(); ++i) {
                const int hi = (i + 1 < layers.size()) ? layers[i + 1] - 1 : 127;
                velRanges[static_cast<int>(i) + 1] = {layers[i], hi};
            }
        } else {
            // No explicit thresholds: split 1..127 evenly among the indices found.
            std::set<int> indices;
            for (const auto& m : matches) indices.insert(m.velIndex);
            const int n = static_cast<int>(indices.size());
            int rank = 0;
            for (int index : indices) {
                const int lo = 1 + rank * 127 / n;
                const int hi = (rank + 1 == n) ? 127 : 1 + (rank + 1) * 127 / n - 1;
                velRanges[index] = {lo, hi};
                ++rank;
            }
            warnings.push_back(tag + "{vel} without \"velocity_layers\": velocity split evenly into " +
                               std::to_string(n) + " ranges");
        }
    }

    // Velocity layers declared but without any file: say so (the nearest layer plays there).
    if (hasVel && !def.velocityLayers.empty()) {
        std::set<int> found;
        for (const auto& m : matches) found.insert(m.velIndex);
        std::string missing;
        for (std::size_t i = 0; i < def.velocityLayers.size(); ++i) {
            if (found.count(static_cast<int>(i) + 1)) continue;
            const std::string name = i < velLabels.size() ? velLabels[i] : std::to_string(i + 1);
            missing += (missing.empty() ? "" : ", ") + name;
        }
        if (!missing.empty())
            warnings.push_back(tag + "no file for velocity layer " + missing +
                               " - the nearest layer plays there; remove it from \"velocity_layers\"");
    }

    // Which files to load: duplicates and files without a velocity range are
    // reported and left out before reading anything.
    struct Job {
        const FileMatch* match;
        std::pair<int, int> velRange{0, 127};
    };
    std::vector<Job> jobs;
    std::set<std::pair<int, int>> used;  // (note, velIndex)
    for (const auto& match : matches) {
        const std::string filename = match.path.filename().string();
        if (!used.insert({match.note, match.velIndex}).second) {
            warnings.push_back(tag + filename + ": same note and velocity as another file - ignored");
            continue;
        }
        Job job{&match};
        if (hasVel) {
            auto it = velRanges.find(match.velIndex);
            if (it == velRanges.end()) {
                warnings.push_back(tag + filename + ": velocity index " + std::to_string(match.velIndex) +
                                   " has no entry in \"velocity_layers\" - ignored");
                continue;
            }
            job.velRange = it->second;
        }
        jobs.push_back(job);
    }

    // Read the files on several threads: the SD card answers faster with a few
    // requests in flight, and the conversion work is shared among the cores.
    std::vector<Zone> zones(jobs.size());
    std::vector<std::string> errors(jobs.size());
    std::vector<char> ok(jobs.size(), 0);
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (std::size_t i = next++; i < jobs.size(); i = next++) {
            ok[i] = loadWav(jobs[i].match->path.string(), zones[i], errors[i]) ? 1 : 0;
        }
    };
    const unsigned threads = std::max(1u, std::min<unsigned>(4, std::min<std::size_t>(jobs.size(), std::max(1u, std::thread::hardware_concurrency()))));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();

    auto instrument = std::make_unique<Instrument>(def.id, def.name);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        if (!ok[i]) {
            warnings.push_back(tag + jobs[i].match->path.filename().string() + ": " + errors[i] + " - ignored");
            continue;
        }
        Zone& zone = zones[i];
        zone.rootNote = jobs[i].match->note;
        if (hasVel) {
            zone.loVel = jobs[i].velRange.first;
            zone.hiVel = jobs[i].velRange.second;
        }
        instrument->addZone(std::move(zone));
    }
    if (instrument->zoneCount() == 0) {
        warnings.push_back(tag + "no readable file - skipped");
        return nullptr;
    }
    instrument->finalize(def.fill);
    return instrument;
}

void InstrumentStore::loadAll(const InstrumentsConfig& config, std::vector<std::string>& warnings) {
    for (const auto& def : config.instruments) {
        auto instrument = load(def, config.mediaRoot, warnings);
        if (!instrument) continue;
        const std::size_t bytes = instrument->memoryBytes();
        if (budget_ != 0 && total_ + bytes > budget_) {
            warnings.push_back("instrument " + def.id + ": " + std::to_string(bytes >> 20) +
                               " MB would exceed the RAM budget - skipped");
            continue;
        }
        add(std::move(instrument));
    }
}

void InstrumentStore::add(std::unique_ptr<Instrument> instrument) {
    total_ += instrument->memoryBytes();
    instruments_.push_back(std::move(instrument));
}

const Instrument* InstrumentStore::find(const std::string& id) const {
    for (const auto& instrument : instruments_)
        if (instrument->id() == id) return instrument.get();
    return nullptr;
}

} // namespace mbx
