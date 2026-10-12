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

### Buttons and LEDs

Buttons on **74HC165** shift registers, LEDs on **74HC595**, both on SPI0 with a shared latch.
Each 165 + 595 pair adds 8 buttons and 8 LEDs (3 pairs = 24, 4 pairs = 32). Supply 3.3 V.

| Signal | Raspberry Pi | 74HC165 | 74HC595 |
| --- | --- | --- | --- |
| Clock | SCLK, GPIO11 (23) | CLK (2) | SRCLK (11) |
| Data in | MISO, GPIO9 (21) | QH (9) of the first 165 | — |
| Data out | MOSI, GPIO10 (19) | — | SER (14) of the first 595 |
| Latch | GPIO25 (22) | SH/LD (1) | RCLK (12) |
| Output enable | GPIO12 (32) | — | OE (13), with 10 kΩ to 3.3 V |
| — | — | CLK INH (15) to GND | SRCLR (10) to 3.3 V |

OE (13) of all the 595s goes to GPIO12, with a 10 kΩ pull-up: the LEDs stay dark from power on
until the panel software has written the chain, and a PWM on OE sets their brightness
(`panel.chain.brightness`).

Chaining: QH (9) of each 165 goes to SER (10) of the next one; QH' (9) of each 595 goes to
SER (14) of the next one. The first 165 is the one on MISO, the first 595 the one on MOSI.

Numbering: button 1 is input A of the first 165, button 8 its input H, button 9 input A of
the second 165, and so on. LED 1 is output QA of the first 595, in the same order.

- Buttons to GND, each input with 10 kΩ to 3.3 V.
- LEDs from the 595 outputs to GND, each with a resistor (1 kΩ at 3.3 V; low-current LEDs).
- 100 nF on the supply of each chip.

What each button does and what each LED shows is set in `engine.json`
(`panel.chain.buttons` and `panel.chain.leds`, see `software/control/README.md`). Default:
button 1 ok, 2 back, 3 Panic, 4 Wi-Fi, 5–24 presets 1–20; each LED shows the state of its
button.

The status LED is not on the chain: it has its own GPIO, so it shows the engine state even
when the chain is not working.

### Encoder, footswitch and status LED

| Function | GPIO (pin) |
| --- | --- |
| Volume encoder A | 5 (29) |
| Volume encoder B | 6 (31) |
| Effect switch footswitch (jack, momentary) | 23 (16) |
| Status LED (330 Ω to GND) | 24 (18) |
| GND | 14, 20, 30, 34, 39 |

Encoder and footswitch to GND (Pi internal pull-ups). The encoder push button goes on the
chain like the other buttons (default: button 1, "ok"). Footswitch on a jack: 1 kΩ in series
towards the GPIO and 100 nF to GND near the Pi, against noise and discharges on the long cable.

## GPIO summary

| GPIO (pin) | Use |
| --- | --- |
| 5, 6 (29, 31) | Volume encoder |
| 8, 9, 10, 11 (24, 21, 19, 23), 25 (22) | Button and LED chain (SPI0 + latch; CE0, GPIO8, is taken by the SPI driver) |
| 12 (32) | LED output enable and brightness (595 OE) |
| 14, 15 (8, 10) | UART, MIDI DIN |
| 16, 17 (36, 11) | DAC A and B mute |
| 18, 19, 21 (12, 35, 40) | I2S |
| 23 (16) | Effect switch footswitch |
| 24 (18) | Status LED |
| 26 (37) | Shutdown button, to GND |
