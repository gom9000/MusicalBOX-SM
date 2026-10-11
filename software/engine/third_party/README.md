# Third-party libraries

Copied unmodified from their upstream repositories. Do not edit these files:
update them by copying a newer release.

| Library | Version | Files | License |
| --- | --- | --- | --- |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | `miniaudio/miniaudio.h` | Public domain or MIT-0 (`miniaudio/LICENSE`) |
| [dr_wav](https://github.com/mackron/dr_libs) | 0.14.6 | `dr_libs/dr_wav.h` | Public domain or MIT-0 (`dr_libs/LICENSE`) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | `nlohmann/json.hpp` | MIT (`nlohmann/LICENSE.MIT`) |
| [RtMidi](https://github.com/thestk/rtmidi) | 6.0.0 | `rtmidi/RtMidi.h`, `rtmidi/RtMidi.cpp` | MIT-style (`rtmidi/LICENSE`) |

The implementations of the header-only libraries are compiled once, in
`src/third_party_impl.cpp`.
