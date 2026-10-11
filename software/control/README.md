# control

`mbx-control` is the companion service of the engine:
- the **web editor**, opened from a phone on the box's Wi-Fi network;
- the **front panel**: buttons, Wi-Fi switch and LEDs on the Pi's GPIO.

Python 3. The web editor uses only the standard library; the front panel uses `gpiozero`
(`sudo apt install python3-gpiozero`, preinstalled on Raspberry Pi OS). Without `gpiozero` the
panel is off and the editor works normally.

```
mbx_control.py     HTTP server and API, talks to mbx-engine through its control socket
panel.py           front panel (GPIO)
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

Pins and options are in `engine.json`, object `"panel"`:

| Key | Default | Use |
| --- | --- | --- |
| `panic` | 22 | Panic button |
| `fx_switch` | 23 | Effect switch footswitch |
| `wifi_switch` | 5 | Wi-Fi switch: closed = Wi-Fi off |
| `wifi_led` | 6 | Wi-Fi LED |
| `status_led` | 24 | Status LED |
| `hotspot` | `"MusicalBOX-SM"` | NetworkManager connection of the box's network |
| `enabled` | true | false = no front panel |

Status LED: slow blink = engine loading; steady = ready; steady with a short dark blip every
3 s = ready with warnings (listed in Sistema); fast blink = engine not running or not answering.

Wi-Fi LED: on = the box's network is active; slow blink = radio on, network starting;
off = Wi-Fi off.

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
