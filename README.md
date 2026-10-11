# MusicalBOX SM (Stage Module)
**MusicalBOX SM** is a sample expander designed for Raspberry Pi 4 and optimized for live stage. Built in C++20 with a lock-free architecture, it provides flexible polyphonic layer management, a programmable effects chain, and real-time control via MIDI and Web UI.

![MusicalBOX-SM](resources/musicalbox-sm.jpg)


## Features
- **Real-Time C++20 Engine**: Low latency audio processing with voice management, ADSR envelopes, sample looping, and velocity mapping.
- **Integrated Effects Chain**: Built-in effects per program (`leslie`, `fuzz`, `chorus`, `phaser`, `echo`, `room`, `tone`, `width`, `tremolo`) featuring dynamic parameters and live switching.
- **MIDI Connectivity**: Native support for USB MIDI controllers and hardware MIDI DIN interfaces via the Raspberry Pi dedicated UART.
- **Integrated Web Editor**: Mobile-friendly configuration interface accessible over Wi-Fi (via the Pi local access point) to tweak presets, programs, layers, and effects on the fly.
- **Samples**: Supports 8, 16, 24, or 32-bit WAV files (mono or stereo) at any sample rate, utilizing loop points. Multiple velocity layers per note are supported via numbers or labels in filenames.
- **Sounds (Programs)**: Up to 3 layers, layered or split by key zones, featuring transposition, volume, ADSR envelope, velocity curves (linear or exponential), and organ mode (held notes repeat the sample loop). Layers inherit parameters from the program unless explicitly overridden.
- **Per-Program Effects**: Equalizer, tape echo, chorus/ensemble, phaser, rotary speaker, tremolo, octave fuzz, stereo width, and room reverb. Each sound includes a switch that toggles one or more parameters with a single button press (e.g., rotary speaker speed).
- **Up to 128 Presets**: as many as defined in `presets.json`, recalled via MIDI Program Change (PC 0–127 → presets 1–128).
- **Audio Output**: 64 voices (configurable), 128-sample blocks (~8 ms buffer), master volume with integrated limiter to eliminate clipping on dense chords.
- **Dual Outputs**: One or two PCM5102A DACs. Each program selects output routing (A, B, or both). DACs remain muted until the audio engine is fully initialized.
- **Web Editor**: Smartphone-accessible Wi-Fi interface for managing keys, sounds, layers, and effects with immediate preview and automatic backup generation on save.
- **Front Panel**: Panic button, effect switch footswitch, Wi-Fi toggle, status LED, and Wi-Fi LED.
- **Recording**: Direct stereo WAV recording of live performances without external cabling.
- **Read-Only System**: Safely power down without risking SD card corruption.


## Hardware
- Raspberry Pi 4.
- Three PCM5102A I2S DAC modules (Outputs A, B, and Headphone).
- Optoisolated MIDI DIN interface connected via UART (GPIO15).
- Panel interface via GPIO.

Wiring, jumpers and parts are described in [hardware/README.md](hardware/README.md).


## Prerequisites
* C++20 compatible compiler
* CMake 3.16 or higher
* ALSA development libraries (`libasound2-dev` on Debian/Ubuntu systems)


## Build and Test
Requires a C++20 compiler, CMake 3.16+, and ALSA headers. All other required libraries are bundled under `software/engine/third_party/`.

```bash
# Install dependencies (Raspberry Pi OS / Debian / Ubuntu)
sudo apt install -y build-essential cmake libasound2-dev

# Build the engine (all commands run from the software folder)
cd software
cmake -S engine -B build
cmake --build build -j4

# Run unit tests
ctest --test-dir build
```


## Quick Start
To verify system functionality without real sample libraries (from the `software/` folder):

```bash
# Generate synthetic test samples
python3 tools/make_test_instrument.py

# Validate the configuration
./build/mbx-engine --check

# List available audio devices and MIDI ports
./build/mbx-engine --list-devices
./build/mbx-engine --list-midi

# Launch the engine using the startup preset
./build/mbx-engine
```

While running, interactive console commands can be used directly (`1`-`128` for preset selection, `w` to toggle effect switches, `s` for status, `q` to quit).


## Configuration
System behavior and sound architectures are defined by JSON files in the `software/config/` directory:

* `engine.json`: Core engine parameters, audio buffer sizes, MIDI channel, and serial port settings.
* `instruments.json`: Sample mappings, zone definitions, and filename patterns.
* `programs.json`: Sound program definitions, layer setups, and effects chains.
* `presets.json`: Mapping of programs to presets (1–128; only the defined ones are needed).

Validate the configuration structure anytime by running `./build/mbx-engine --check`.

### engine.json
| Key | Default | Description |
| --- | --- | --- |
| `audio.device` | `""` | Audio card name or partial match (`mbx-engine --list-devices`); empty = default |
| `audio.sample_rate` | 48000 | Sample rate in Hz |
| `audio.period_frames`, `audio.periods` | 128, 3 | Audio block size: smaller = lower latency, higher CPU load |
| `master_volume_db` | −6 | Master volume level at startup |
| `polyphony` | 64 | Simultaneous voices; a single note consumes one voice per active layer |
| `max_layers` | 3 | Maximum layers per program |
| `ram_budget_percent` | 70 | RAM quota allocated for samples; 0 = unlimited |
| `midi_channel` | `"omni"` | MIDI channel 1–16 or `"omni"` |
| `midi_serial` | `"/dev/serial0"` | MIDI DIN serial port; `""` = disabled |
| `record_folder` | `"/tmp"` | Target directory for audio recordings |
| `control_socket` | `"/tmp/mbx-engine.sock"` | Unix socket for the web editor; `""` = disabled |
| `dac_mute` | `a` 16, `b` 17 | BCM GPIO pins for DAC A and B mute controls; `{"enabled": false}` if DACs are permanently unmuted |
| `panel` | see below | Front panel pin assignments |

Panel configuration (`"panel"`, BCM numbering): `panic` 22, `fx_switch` 23, `wifi_switch` 5, `wifi_led` 6, `status_led` 24, `hotspot` (network name, default `"MusicalBOX-SM"`), `enabled` (true).

### instruments.json
```json
{ "id": "STRINGS", "name": "Strings", "path": "strings/ensemble", "files": "{note}_*.wav" }
```

- `path` is relative to `media_root` (default `../media`, located alongside `config/`).
- `files` defines the filename structure:
  - `{note}` is the MIDI note number;
  - `{name}` is the note name (`C3`, `F#2`, etc.);
  - `{vel}` is the velocity tier, either numeric or a label defined in `velocity_layers`;
  - `*` acts as a wildcard matching any text.
- `name_octave` selects the naming convention: `"C3"` (default, middle C = C3) or `"C4"`.
- `fill`: with `"nearest"` (default) unassigned keys fall back to the closest available pitched sample; with `"none"` unassigned keys remain silent.

### programs.json
```json
{
  "id": "strings_choir", "name": "Strings + Choir", "output": "A",
  "loop": "on",
  "adsr": { "attack_ms": 30, "release_ms": 500 },
  "velocity": { "mode": "on", "levels": 8, "curve": "exp" },
  "layers": [
    { "instrument": "BASS",    "keys": ["C-2", "B2"] },
    { "instrument": "STRINGS", "keys": ["C3", "G8"] },
    { "instrument": "CHOIR",   "keys": ["C3", "G8"], "gain_db": -4 }
  ],
  "fx": [ { "type": "chorus", "params": { "mode": "ensemble" } } ]
}
```

| Field | Default | Description |
| --- | --- | --- |
| `output` | `"A"` | Output destination: `"A"`, `"B"`, or `"AB"` |
| `gain_db` | 0 | Program volume adjustment |
| `adsr` | 2 ms / 0 / 1 / 150 ms | `attack_ms`, `decay_ms`, `sustain` (0–1), `release_ms` |
| `velocity` | `off` | `mode` on/off, `levels` (1–127), `curve` `linear` or `exp` (40 dB range) |
| `loop` | `off` | `on` = organ mode, held notes continuously loop the sample |
| `layers[]` | | `instrument`, `keys` [low, high], `transpose` (semitones), `gain_db`, plus optional layer-specific `adsr` / `velocity` / `loop` |
| `fx` | | Effects chain, see the "Effects" section below |

### presets.json

```json
{ "version": 1,
  "startup": 1,
  "presets": { 
    "1": "strings_choir",
    "2": "piano" 
  }
}
```


## Sample Utilities
Python helper tools located in the `software/tools/` directory:

| Command | Purpose |
| --- | --- |
| `wav_inspect.py media` | Generates a sample inventory and RAM usage report |
| `make_loops.py media/<folder>` | Detects loop points in unlooped files and writes copies to `<folder>-loop` |
| `sf2_extract.py file.sf2 --out media/<folder>` | Extracts SoundFont samples as WAV files complete with loop points |
| `wav_tone.py IN OUT --high-cut 2500` | Creates a pre-filtered brighter or darker copy of a sample folder (`--high-cut`, `--low-cut`, `--treble-db`, `--bass-db`) |
| `make_synth_lead.py media/synth/lead` | Generates monophonic synth leads (sawtooth or pulse wave, resonant filter, formant peak via `--cutoff`, `--resonance`, `--peak-hz`, `--peak-db`, `--wave pulse --duty`) |
| `make_test_instrument.py` | Generates a synthetic test instrument in `media/test/tone` |

`make_loops.py`, `wav_tone.py`, and `make_synth_lead.py` require `python3-numpy`.


## Effects
Each program features an ordered effects chain applied to the summed output of its layers.
- Unspecified parameters fall back to their defaults.
- Out-of-bounds parameter values are automatically clamped with a warning message.
- `"bypass": true` temporarily disables an effect without removing it.
- During program changes, active notes are truncated and previous effects (including echo/reverb tails) fade out smoothly over 30 ms to prevent audio clicks.

```json
"fx": [
  { "type": "chorus", "params": { "mode": "ensemble" } },
  { "type": "echo",   "params": { "time_ms": 420, "feedback": 0.3, "mix": 0.2 } }
]
```

| Effect | Parameters (defaults) |
| --- | --- |
| `tone` | Equalizer: `low_cut_hz` (0 = off), `bass_db` (0) at `bass_hz` (120), `mid_db` (0) at `mid_hz` (1000), `treble_db` (0) at `treble_hz` (3500), `high_cut_hz` (0 = off), `gain_db` (0) |
| `echo` | Tape echo: `time_ms` (350), `feedback` (0.35), `mix` (0.3), `tone_hz` (2800: darkens repeated echoes), `low_cut_hz` (120), `wow` (0.3: tape pitch modulation), `drive` (0.3: saturation), `ping_pong` (false) |
| `chorus` | `mode` `"ce2"` (classic chorus) or `"ensemble"` (three-line ensemble effect); `rate_hz` (0.8 / 0.6), `depth` (0.5), `mix` (0.5 / 0.7) |
| `phaser` | 4-stage phaser: `rate_hz` (0.5), `depth` (1), `feedback` (0.2), `mix` (0.5), `stages` (4), `stereo` (false) |
| `leslie` | Rotary speaker: `speed` `"slow"` or `"fast"`, `depth` (1, up to 2), `spread` (0.8: stereo microphone distance), `cabinet` (1: cabinet and room reflections; 0 = direct sound only), `top_hz` (2800: horn high-frequency cutoff), `drive` (0: amplifier saturation), `mix` (1) |
| `fuzz` | Monophonic octave fuzz: `drive` (0.6), `octave` (0.5), `bias` (0.2: asymmetry), `tone_hz` (2500), `low_cut_hz` (150: excludes low frequencies from fuzz distortion), `blend` (1 = full fuzz; lower values mix in clean signal), `level_db` (0) |
| `tremolo` | `rate_hz` (5), `depth` (0.5), `shape` `"sine"` / `"triangle"` / `"square"`, `stereo` (false; true = panning tremolo) |
| `width` | Stereo widening for mono sources: `amount` (0.5), `time_ms` (12), `low_cut_hz` (250: frequencies below this remain centered). Summing to mono perfectly reconstructs the original signal |
| `room` | Small stereo decorrelated room reverb: `level` (0.5), `time_s` (0.7), `damp_hz` (3000: darker decay tail), `predelay_ms` (5), `low_cut_hz` (250) |

### Switch
Effects can define a `switch`: a parameter that toggles between two distinct values each time the physical footswitch (or web UI button / console `w` command) is pressed.
- The initial value is active when the program is loaded.
- A single press simultaneously switches all switchable parameters within the program.
- Any parameter can be automated via switch, including `bypass`.

```json
"fx": [
  { "type": "leslie", "params": { "drive": 0.2 }, "switch": ["speed", "slow", "fast"] },
  { "type": "echo",   "params": { "time_ms": 300 }, "switch": ["bypass", true, false] }
]
```


## Raspberry Pi Installation
1. **`/boot/firmware/config.txt`**: Append the configuration lines from [software/system/config.txt](software/system/config.txt) to configure the DACs, MIDI DIN UART, and shutdown button.
2. **`/boot/firmware/cmdline.txt`**: Remove `console=serial0,115200`.
3. **Free up UART for MIDI**:
   ```bash
   sudo systemctl disable --now serial-getty@ttyAMA0 hciuart
   ```
4. **Build and Deploy**: Complete the build steps outlined above, then install system services and set up the local Wi-Fi hotspot:
   ```bash
   cd software
   sh system/install-services.sh         # enables engine & web editor on boot
   sh system/mbx-hotspot.sh on PASSWORD    # creates Wi-Fi network "MusicalBOX-SM" (min. 8 characters)
   sh system/mbx-readonly.sh on            # enables read-only filesystem protection, then: sudo reboot
   ```

`mbx-hotspot.sh on` disconnects the Pi from external Wi-Fi networks and starts the local access point at `10.42.0.1`.  
`mbx-hotspot.sh off` restores previous network settings.

The engine service locks the CPU to maximum performance frequency and runs with real-time scheduling priority. View live logs with `journalctl -u mbx-engine -f`.


## About & License
**Author**: Alessandro Fraschetti (gom9000).  
**Credits**: Inspired by *SamplerBox* by Joseph Ernest.  
**License**: This project is licensed under the [MIT License](LICENSE), which covers the source code, documentation, schematics and PCB layouts in this repository. Third-party libraries in `software/engine/third_party/` keep their own licenses (see [software/engine/third_party/README.md](software/engine/third_party/README.md)). Audio samples are not included.
