# Hardware

Schematics and PCB layouts of MusicalBOX SM, and how the parts connect to the Raspberry Pi 4.
Pins are given as BCM GPIO numbers, with the physical header pin in brackets.

## Audio outputs

Three **PCM5102A** DAC modules (the common purple "GY-PCM5102" board) on the Pi's I2S bus,
device tree overlay `hifiberry-dac`:

| DAC | Use | Mute (XSMT) |
| --- | --- | --- |
| A | Line output A | GPIO16 (36), driven by the engine |
| B | Line output B | GPIO17 (11), driven by the engine |
| Headphone | Headphone monitor | tied high: always on |

All three modules receive the same stereo stream. A program chooses where it sounds with
`output` (`A`, `B` or `AB`): the engine unmutes the matching DACs through their XSMT pins. The
headphone DAC is always on, so it carries every program whatever its output.

A full-scale PCM5102A output is about 2.1 V RMS, a full line level. The master volume is set in
the engine (`master_volume_db` in `engine.json`) and on the mixer.

### Wiring (each module)

| Module | Raspberry Pi |
| --- | --- |
| VIN | 5 V (2 or 4); the module has its own 3.3 V regulator |
| GND | GND (6) |
| BCK | GPIO18 (12) |
| LRCK | GPIO19 (35) |
| DIN | GPIO21 (40) |
| SCK | GND on the module: the PCM5102A derives its clock from BCK |
| XSMT, DAC A | GPIO16 (36) through 1 kΩ in series, 10 kΩ to GND at the module |
| XSMT, DAC B | GPIO17 (11) through 1 kΩ in series, 10 kΩ to GND at the module |

BCK, LRCK and DIN are wired in parallel to the three modules. With long traces, a 33–47 Ω
resistor in series with BCK near the Pi keeps the clock edges clean.

Solder jumpers on the back of each module: H1L (FLT), H2L (DEMP) and H4L (FMT) closed to **L**.
H3L (XSMT) is **open** on DACs A and B, where the Pi drives it, and closed to **H** on the
headphone DAC. With a single line DAC that is always on, close its H3L to H and set
`"dac_mute": {"enabled": false}` in `engine.json`.

### Muting at power on

The 10 kΩ pull-down keeps DACs A and B muted until the engine is ready (the Pi also pulls
GPIO16 and 17 down at boot). The engine:
- opens the pins low;
- unmutes the DAC of the current program 150 ms after the audio starts;
- follows each program change with the delay of the audio buffer (about 8 ms), so the switch
  happens when the new sound reaches the DACs;
- mutes both before the audio stops.

The PCM5102A ramps the volume itself on mute and unmute, so there are no clicks. The engine
service runs in the `gpio` group (set by `system/install-services.sh`).

### Headphone amplifier

Headphone DAC → dual logarithmic potentiometer (10–50 kΩ, "A" taper) → NE5532 stage →
headphone jack. The volume control is analog and independent of the line outputs.

## MIDI DIN

Interface: [mob-if-midi](https://github.com/gom9000/mobs-library/tree/master/mobs/mob-if-midi)
(6N138 on the input, 74LS14 for OUT and THRU), on the Pi's UART:

| mob-if-midi | Raspberry Pi |
| --- | --- |
| 3V3 (PWR2) | 3.3 V (1) |
| 5V (PWR2) | 5 V (2) |
| GND | GND (6) |
| Rx | GPIO15, RXD0 (10) |
| Tx | GPIO14, TXD0 (8), not used |

Jumper J1 goes to 3V3: the 6N138 output must never put 5 V on GPIO15. If wrong bytes arrive
(`fe` counter in `/proc/tty/driver/ttyAMA`), add 4.7–10 kΩ between pin 7 of the 6N138 and GND.

UART test without the interface: link pin 8 to pin 10, run
`stty -F /dev/serial0 38400 raw -echo` and `echo -n test > /dev/serial0` with a reader open;
the `tx` and `rx` counters in `/proc/tty/driver/ttyAMA` rise together.

GPIO14 and GPIO15 belong to the UART: no fan (`gpio-fan`) or anything else on those pins.

## Front panel

Buttons and switch to GND (Pi internal pull-ups), LEDs with 330 Ω to GND.

| Function | GPIO (pin) |
| --- | --- |
| Panic | 22 (15) |
| Effect switch footswitch (jack, momentary) | 23 (16) |
| Wi-Fi switch (closed = Wi-Fi off) | 5 (29) |
| Wi-Fi LED | 6 (31) |
| Status LED | 24 (18) |
| GND | 14, 20, 30, 34, 39 |

Footswitch on a jack: 1 kΩ in series towards the GPIO and 100 nF to GND near the Pi, against
noise and discharges on the long cable.

## GPIO summary

| GPIO (pin) | Use |
| --- | --- |
| 5, 6, 22, 23, 24 | Front panel |
| 14, 15 (8, 10) | UART, MIDI DIN |
| 16, 17 (36, 11) | DAC A and B mute |
| 18, 19, 21 (12, 35, 40) | I2S |
| 8–11, 25 | SPI and latch for front-panel expansion (shift registers) |
| 26 (37) | Shutdown button, to GND |
