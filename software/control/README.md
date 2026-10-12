# control

`mbx-control` is the companion service of the engine:
- the **web editor**, opened from a phone on the box's Wi-Fi network;
- the **front panel**: buttons and LEDs on shift registers, volume encoder, footswitch.

Python 3. The web editor uses only the standard library; the front panel uses `gpiozero`
(`sudo apt install python3-gpiozero`, preinstalled on Raspberry Pi OS). Without `gpiozero` the
panel is off and the editor works normally.

```
mbx_control.py     HTTP server and API, talks to mbx-engine through its control socket
panel.py           front panel (SPI chain, encoder, footswitch, LEDs)
web/               the editor page (HTML, CSS, JavaScript, no libraries)
```

Run by hand, with the engine already running (from the `software/` folder):

```bash
python3 control/mbx_control.py                 # http://<Pi address>:8080
python3 control/mbx_control.py --no-panel      # while the service owns the GPIO
```

As a service on port 80: `system/install-services.sh`. The box's own Wi-Fi network:
`system/mbx-hotspot.sh` (the editor is then at `http://10.42.0.1`).

## Web editor

The interface is in Italian. Its pages:

- **Pannello** (panel): the presets in rows of 8. *Suona* (play) selects a preset; *Assegna*
  (assign) sets the program of each preset and the one selected at power on (★), and adds new
  presets. Effect switch, Panic and master volume.
- **Suoni** (sounds): the programs. Name, volume, output, envelope, velocity, loop, layers and
  the effects chain with its switch. Every change is heard at once; an on-screen keyboard plays
  the sound.
- **Strumenti** (instruments): the instruments in memory and the RAM they use.
- **Sistema** (system): engine status, warnings, recordings (start, stop, download, delete)
  and backups.

Changes become permanent with *Salva* (save); *Annulla* (revert) goes back to the saved files.
On each save the previous files are copied to `config/backup/`: the last 30 are kept and can be
restored from Sistema. On a read-only system the editor unlocks the disk for the time of the
save.

## Front panel

Pins and options are in `engine.json`, object `"panel"` (BCM GPIO numbers; `null` = not
connected). Every part is optional: what is not connected is simply not used.

| Key | Default | Use |
| --- | --- | --- |
| `fx_switch` | 23 | Effect switch footswitch |
| `encoder` | `[5, 6]` | Volume encoder, pins A and B |
| `volume_step_db` | 1 | Volume change per encoder step (double when turning fast) |
| `status_led` | 24 | Status LED, on its own GPIO so it works whatever happens to the chain |
| `wifi_led` | `null` | Wi-Fi LED on a GPIO, for a panel without the chain |
| `wifi_at_boot` | false | Wi-Fi at power on; false = off until the `wifi` button is pressed |
| `hotspot` | `"MusicalBOX-SM"` | NetworkManager connection of the box's network |
| `chain` | see below | Buttons and LEDs on shift registers |
| `enabled` | true | false = no front panel |

The encoder changes the master volume of the engine (−60 to +6 dB), like the slider of the
editor. The change is not saved: at power on the volume is `master_volume_db`.

### Buttons and LEDs (`"chain"`)

Buttons on 74HC165 and LEDs on 74HC595 on SPI, with a shared latch (wiring in
`hardware/README.md`). The SPI bus must be enabled (`dtparam=spi=on`) and the service in the
`spi` group (`system/install-services.sh`).

| Key | Default | Use |
| --- | --- | --- |
| `enabled` | false | true when the board is connected |
| `device` | `"/dev/spidev0.0"` | SPI device |
| `latch` | 25 | Latch GPIO (165 SH/LD, 595 RCLK) |
| `oe` | 12 | 595 output enable GPIO: LEDs dark until the first write, then on; `null` = OE wired to GND |
| `brightness` | 100 | LED brightness in % (PWM on `oe`; 100 = steady) |
| `length` | 3 | 165 + 595 pairs: 8 buttons and 8 LEDs each |
| `buttons` | see below | Button number → action |
| `leds` | `{}` | LED number → what it shows; LED n shows button n's action unless set here |

Actions:

| Action | Button | LED |
| --- | --- | --- |
| `preset N` | selects preset N (1–128) | on while preset N plays |
| `preset +`, `preset -` | next / previous defined preset | — |
| `switch` | effect switch | on in position B |
| `panic` | all sound off | — |
| `wifi` | Wi-Fi on / off | on = network active, slow blink = starting, off = Wi-Fi off |
| `record` | starts / stops a recording | blinks while recording |
| `volume +`, `volume -` | master volume up / down one step | — |
| `ok`, `back` | menu keys | — |
| `status` | — | engine status, as the status LED |

Default buttons: 1 `ok` (the encoder push button), 2 `back`, 3 `panic`, 4 `wifi`,
5–24 `preset 1` … `preset 20`.

```json
"panel": {
  "chain": {
    "enabled": true,
    "length": 4,
    "buttons": { "1": "ok", "2": "back", "3": "panic", "4": "wifi", "5": "record",
                 "9": "preset 1", "10": "preset 2", "32": "preset 24" },
    "leds": { "6": "switch" }
  }
}
```

A `buttons` object replaces the default one entirely.

Wi-Fi: the box starts with Wi-Fi off and is reached by Ethernet; the `wifi` button turns the
box's own network on and off, and at the next power on it is off again. With
`"wifi_at_boot": true` it starts on.

Status LED: slow blink = engine loading; steady = ready; steady with a short dark blip every
3 s = ready with warnings (engine warnings, listed in Sistema, or a button chain that is
enabled but not working); fast blink = engine not running or not answering.

If the chain reads every input low (board missing or not powered), the buttons are ignored,
the log says so and the status LED shows a warning.

## API

| Method | Path | Does |
| --- | --- | --- |
| GET | `/api/state` | configuration files, schemas, instruments, engine bank, status, backups |
| GET | `/api/status` | engine status (preset, voices, load, peak, MIDI) and CPU temperature |
| GET | `/api/warnings` | warnings of the startup and of the last preview |
| GET | `/api/recordings` | recordings in the engine's `record_folder`, newest first |
| GET | `/api/recordings/<name>` | downloads a recording |
| POST | `/api/cmd` | `{"cmd": "preset", "slot": 3}`, `switch`, `volume` (`db`), `panic`, `note` (`key`, `velocity`, 0 = release), `record` (`action` start/stop) |
| POST | `/api/preview` | `{programs, presets}`: the engine plays them at once, without saving |
| POST | `/api/save` | checks, copies the current files to `config/backup/`, writes, reloads |
| POST | `/api/revert` | the engine goes back to the saved files |
| POST | `/api/restore` | `{"backup": "YYYYMMDD-hhmmss"}`: restores a backup |
| POST | `/api/recordings/delete` | `{"name": "..."}`: deletes a recording |
