"""Front panel of MusicalBOX SM, handled by mbx-control.

- Button and LED chain: 74HC165 (buttons) and 74HC595 (LEDs) on SPI, with a
  shared latch; the 595 output enable on a GPIO keeps the LEDs dark until the
  first write and sets their brightness (PWM). Each button runs an action from engine.json; each LED shows
  the state of an action ("preset 3" lit while preset 3 plays...).
- Volume encoder on two GPIO: master volume, 1 dB per step. Not saved: at
  power on the volume is master_volume_db of engine.json.
- Effect-switch footswitch on its own GPIO.
- Status LED on its own GPIO, so it works whatever happens to the chain;
  optional Wi-Fi LED on a GPIO, for a panel without the chain.

Settings come from the "panel" object of engine.json (see control/README.md).
Every part is optional: a missing chain, encoder or LED changes nothing else.
Without gpiozero, or on a PC, the panel is simply off.

Status LED:  slow blink = engine loading,  steady = ready,
             steady with a short dark blip every 3 s = ready, with warnings
             (engine warnings, or the button chain not working),
             fast blink = engine not running / not answering.
Wi-Fi LED:   on = own network active, slow blink = radio on but network not
             (yet) up, off = radio off.
Wi-Fi:       off at power on (the box is reached by Ethernet); the "wifi"
             button turns it on and off. "wifi_at_boot": true leaves it on.
"""

import array
import fcntl
import os
import queue
import subprocess
import threading
import time

NMCLI = "/usr/bin/nmcli"
VOLUME_MIN_DB, VOLUME_MAX_DB = -60.0, 6.0

DEFAULT_BUTTONS = {"1": "ok", "2": "back", "3": "panic", "4": "wifi"}
DEFAULT_BUTTONS.update({str(n): f"preset {n - 4}" for n in range(5, 25)})

DEFAULTS = {
    "enabled": True,
    "fx_switch": 23,           # footswitch (momentary) to GND
    "encoder": [5, 6],         # volume encoder A, B; null = none
    "volume_step_db": 1.0,
    "status_led": 24,          # status LED, on its own GPIO
    "wifi_led": None,          # optional Wi-Fi LED on a GPIO (the chain can show it too)
    "wifi_at_boot": False,     # Wi-Fi at power on; then the "wifi" button
    "hotspot": "MusicalBOX-SM",  # NetworkManager connection of the box's own network
    "loading_timeout_s": 180,    # engine running this long without answering = error
    "chain": {
        "enabled": False,
        "device": "/dev/spidev0.0",
        "latch": 25,
        "oe": 12,                # 595 OE (active low, 10 kOhm pull-up): LEDs dark until the first write; PWM = brightness
        "brightness": 100,       # LED brightness, %
        "length": 3,             # 74HC165 + 74HC595 pairs: 8 buttons and 8 LEDs each
        "speed_hz": 500000,
        "buttons": DEFAULT_BUTTONS,
        "leds": {},              # LED n shows button n's action, unless set here
    },
}

ACTIONS = ("ok", "back", "panic", "switch", "wifi", "record", "volume +", "volume -", "preset +", "preset -")


def parse_action(text):
    """'preset 3' -> ('preset', 3); 'panic' -> ('panic', None); invalid -> None."""
    text = " ".join(str(text or "").split()).lower()
    if not text:
        return None
    if text in ACTIONS or text == "status":
        return (text, None)
    parts = text.split(" ")
    if len(parts) == 2 and parts[0] == "preset" and parts[1].isdigit() and 1 <= int(parts[1]) <= 128:
        return ("preset", int(parts[1]))
    return None


def engine_process_running():
    """PID of an mbx-engine process (started by systemd or by hand), 0 if none."""
    try:
        for pid in os.listdir("/proc"):
            if pid.isdigit():
                try:
                    with open(f"/proc/{pid}/comm", encoding="utf-8") as f:
                        if f.read().strip() == "mbx-engine":
                            return int(pid)
                except OSError:
                    pass
    except OSError:
        pass
    return 0


# ---------------------------------------------------------------------------
# LED patterns: ("off",), ("on",), ("blink", on_s, off_s)
# ---------------------------------------------------------------------------

OFF, ON = ("off",), ("on",)


def blink(on_s, off_s):
    return ("blink", on_s, off_s)


def led_level(pattern, t):
    if pattern[0] == "on":
        return True
    if pattern[0] == "blink":
        period = pattern[1] + pattern[2]
        return (t % period) < pattern[1]
    return False


# ---------------------------------------------------------------------------
# SPI chain (no library: the spidev ioctls)
# ---------------------------------------------------------------------------

SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04
SPI_IOC_MESSAGE_1 = 0x40206B00


class SpiChain:
    """74HC165 inputs and 74HC595 outputs on one SPI bus with a shared latch.

    One cycle: latch low (the 165s load the buttons, the 595s hold), latch
    high (the 595s show what the previous transfer shifted in), then one
    transfer reads the buttons and shifts the new LED bytes in.
    Byte k read = 165 number k (k = 0 is the one on MISO), bit 0 = input A.
    LED byte for 595 number k (k = 0 is the one on MOSI) is sent last.
    """

    def __init__(self, device, length, speed_hz, latch):
        self.length = length
        self.speed = speed_hz
        self.latch = latch  # object with .on() / .off()
        self.fd = os.open(device, os.O_RDWR)
        fcntl.ioctl(self.fd, SPI_IOC_WR_MODE, array.array("B", [0]))
        fcntl.ioctl(self.fd, SPI_IOC_WR_MAX_SPEED_HZ, array.array("I", [speed_hz]))
        self.tx = array.array("B", bytes(length))
        self.rx = array.array("B", bytes(length))

    def cycle(self, led_bytes):
        self.latch.off()
        self.latch.on()
        for i, b in enumerate(reversed(led_bytes)):
            self.tx[i] = b
        tx_addr, _ = self.tx.buffer_info()
        rx_addr, _ = self.rx.buffer_info()
        transfer = array.array("B", bytes(32))
        transfer[0:8] = array.array("B", tx_addr.to_bytes(8, "little"))
        transfer[8:16] = array.array("B", rx_addr.to_bytes(8, "little"))
        transfer[16:20] = array.array("B", self.length.to_bytes(4, "little"))
        transfer[20:24] = array.array("B", self.speed.to_bytes(4, "little"))
        transfer[26] = 8  # bits per word
        fcntl.ioctl(self.fd, SPI_IOC_MESSAGE_1, transfer)
        return bytes(self.rx)

    def close(self):
        try:
            self.cycle(bytes(self.length))  # LEDs off
            self.cycle(bytes(self.length))
        except OSError:
            pass
        os.close(self.fd)


# ---------------------------------------------------------------------------
# Panel
# ---------------------------------------------------------------------------

class Panel:
    SCAN_S = 0.004       # chain scan period
    DEBOUNCE = 4         # equal reads to accept a change (16 ms)

    def __init__(self, engine, settings, hardware=None):
        """hardware: for tests, an object with button(pin), led(pin),
        output(pin), encoder(a, b), spi(cfg, latch) and nmcli(args)."""
        self.engine = engine
        self.cfg = dict(DEFAULTS)
        user = {k: v for k, v in (settings or {}).items() if not k.startswith("_")}
        chain = dict(DEFAULTS["chain"])
        chain.update({k: v for k, v in (user.pop("chain", None) or {}).items() if not k.startswith("_")})
        self.cfg.update(user)
        self.cfg["chain"] = chain
        self.ok = False
        self.chain_problem = None    # text, when the chain is enabled but not working
        self.lock = threading.Lock()
        self.patterns = {"status": blink(0.5, 0.5), "wifi": OFF}  # LED name -> pattern
        self.engine_state = {}
        self.state = None
        self.wifi_state = None
        self.volume = None           # last volume sent or read, dB
        self.volume_touched = 0.0
        self.last_step = 0.0
        self.actions = queue.Queue()
        self.gpio_leds = {}
        self.chain = None
        self.buttons = {}            # chain input index (0-based) -> action
        self.leds = {}               # chain output index (0-based) -> action
        if not self.cfg.get("enabled", True):
            self.log("front panel disabled in engine.json")
            return
        self.hw = hardware or self._gpiozero()
        if self.hw is None:
            return
        self._setup()

    @staticmethod
    def log(text):
        print(f"mbx-control: {text}", flush=True)

    # --- Hardware --------------------------------------------------------------
    def _gpiozero(self):
        try:
            import gpiozero  # noqa: PLC0415 (only on the Pi)
        except ImportError:
            self.log("gpiozero not available - no front panel (sudo apt install python3-gpiozero)")
            return None

        class Hw:
            def button(self, pin):
                return gpiozero.Button(pin, pull_up=True, bounce_time=0.03)

            def led(self, pin):
                return gpiozero.LED(pin)

            def output(self, pin):
                return gpiozero.DigitalOutputDevice(pin, initial_value=True)

            def enable(self, pin, level):
                """Active-low enable: steady low at 1.0, PWM below."""
                if level >= 1.0:
                    return gpiozero.DigitalOutputDevice(pin, active_high=False, initial_value=True)
                return gpiozero.PWMOutputDevice(pin, active_high=False, initial_value=level, frequency=1000)

            def encoder(self, a, b):
                return gpiozero.RotaryEncoder(a, b, max_steps=0)

            def spi(self, cfg, latch):
                return SpiChain(cfg["device"], int(cfg["length"]), int(cfg["speed_hz"]), latch)

            def nmcli(self, args, sudo=False):
                cmd = (["sudo", "-n"] if sudo else []) + [NMCLI] + args
                return subprocess.run(cmd, capture_output=True, text=True, timeout=10)

        return Hw()

    def _setup(self):
        parts = []
        # Footswitch.
        if self.cfg.get("fx_switch") is not None:
            try:
                self.fx = self.hw.button(self.cfg["fx_switch"])
                self.fx.when_pressed = lambda: self.actions.put(("switch", None))
                parts.append(f"footswitch GPIO {self.cfg['fx_switch']}")
            except Exception as e:  # pin busy, not a Pi...
                self.log(f"footswitch not available ({e})")
        # Encoder.
        enc = self.cfg.get("encoder")
        if enc:
            try:
                self.encoder = self.hw.encoder(enc[0], enc[1])
                self.encoder.when_rotated_clockwise = lambda: self.turn(+1)
                self.encoder.when_rotated_counter_clockwise = lambda: self.turn(-1)
                parts.append(f"volume encoder GPIO {enc[0]}/{enc[1]}")
            except Exception as e:
                self.log(f"volume encoder not available ({e})")
        # LEDs on GPIO.
        for name in ("status", "wifi"):
            pin = self.cfg.get(f"{name}_led")
            if pin is not None:
                try:
                    self.gpio_leds[name] = self.hw.led(pin)
                    parts.append(f"{name} LED GPIO {pin}")
                except Exception as e:
                    self.log(f"{name} LED not available ({e})")
        # Button and LED chain.
        chain = self.cfg["chain"]
        if chain.get("enabled"):
            try:
                latch = self.hw.output(chain["latch"])
                self.chain = self.hw.spi(chain, latch)
                count = 8 * int(chain["length"])
                for key, text in (chain.get("buttons") or {}).items():
                    action = parse_action(text)
                    if str(key).isdigit() and 1 <= int(key) <= count and action:
                        self.buttons[int(key) - 1] = action
                    elif text:
                        self.log(f"panel button {key}: \"{text}\" not understood - ignored")
                for i, action in self.buttons.items():
                    self.leds[i] = action
                for key, text in (chain.get("leds") or {}).items():
                    action = parse_action(text)
                    if str(key).isdigit() and 1 <= int(key) <= count:
                        if action:
                            self.leds[int(key) - 1] = action
                        else:
                            self.leds.pop(int(key) - 1, None)
                parts.append(f"{count} buttons/LEDs on {chain['device']}, latch GPIO {chain['latch']}")
                self.chain.cycle(bytes(int(chain["length"])))  # LEDs off in the 595s ...
                self.chain.cycle(bytes(int(chain["length"])))
                if chain.get("oe") is not None:              # ... then the outputs on
                    level = max(0.05, min(1.0, float(chain.get("brightness", 100)) / 100))
                    self.oe = self.hw.enable(chain["oe"], level)
                    parts.append(f"LED enable GPIO {chain['oe']} at {round(level * 100)}%")
            except Exception as e:
                self.chain_trouble(f"button chain not available ({e})")
                self.chain = None

        self.ok = True
        self.set_wifi(bool(self.cfg.get("wifi_at_boot")))
        threading.Thread(target=self.run_engine, daemon=True).start()
        threading.Thread(target=self.run_actions, daemon=True).start()
        threading.Thread(target=self.run_io, daemon=True).start()
        self.log("front panel: " + (", ".join(parts) if parts else "nothing connected"))

    def chain_trouble(self, text):
        """The chain is enabled but not working: log it, show it on the status LED."""
        if text != self.chain_problem:
            self.log(text)
        self.chain_problem = text
        if self.state == "ready":
            self.set_status("warnings")

    # --- Actions ---------------------------------------------------------------
    def turn(self, direction):
        now = time.monotonic()
        step = float(self.cfg.get("volume_step_db", 1.0))
        if now - self.last_step < 0.04:
            step *= 2  # turning fast
        self.last_step = now
        self.actions.put(("volume", direction * step))

    def run_actions(self):
        while True:
            name, arg = self.actions.get()
            try:
                self.do(name, arg)
            except Exception as e:  # never let a button kill the panel
                self.log(f"panel action {name} failed ({e})")

    def do(self, name, arg):
        if name == "panic":
            self.engine.call({"cmd": "panic"})
        elif name == "switch":
            self.engine.call({"cmd": "switch"})
        elif name == "preset":
            self.engine.call({"cmd": "preset", "slot": arg})
        elif name in ("preset +", "preset -"):
            bank = self.engine.call({"cmd": "bank"})
            defined = [i + 1 for i, p in enumerate(bank.get("presets") or []) if p]
            if defined:
                slot = self.engine_state.get("slot") or 0
                if name == "preset +":
                    nxt = next((s for s in defined if s > slot), defined[0])
                else:
                    nxt = next((s for s in reversed(defined) if s < slot), defined[-1])
                self.engine.call({"cmd": "preset", "slot": nxt})
        elif name == "volume" or name in ("volume +", "volume -"):
            delta = arg if name == "volume" else float(self.cfg.get("volume_step_db", 1.0)) * (1 if name == "volume +" else -1)
            if self.volume is None:
                self.volume = float(self.engine.call({"cmd": "status"}).get("volume_db", -6.0))
            # Steps queued while turning: add them up, send once.
            while True:
                try:
                    more_name, more_arg = self.actions.get_nowait()
                except queue.Empty:
                    break
                if more_name == "volume":
                    delta += more_arg
                else:
                    self.actions.put((more_name, more_arg))
                    break
            self.volume = max(VOLUME_MIN_DB, min(VOLUME_MAX_DB, round(self.volume + delta, 1)))
            self.volume_touched = time.monotonic()
            self.engine.call({"cmd": "volume", "db": self.volume})
        elif name == "wifi":
            self.set_wifi(self.wifi_state == "off")
        elif name == "record":
            recording = self.engine.call({"cmd": "status"}).get("recording")
            self.engine.call({"cmd": "record", "action": "stop" if recording else "start"})
        # "ok" and "back": the menu keys, for the display.

    # --- Wi-Fi -----------------------------------------------------------------
    def set_wifi(self, on):
        try:
            r = self.hw.nmcli(["radio", "wifi", "on" if on else "off"], sudo=True)
            if r.returncode != 0:
                self.log(f"Wi-Fi {'on' if on else 'off'} failed: {r.stderr.strip()}")
            else:
                self.log(f"Wi-Fi {'on' if on else 'off'}")
        except (OSError, subprocess.TimeoutExpired) as e:
            self.log(f"Wi-Fi {'on' if on else 'off'} failed: {e}")
        self.update_wifi_led()

    def update_wifi_led(self):
        try:
            radio = self.hw.nmcli(["radio", "wifi"]).stdout.strip()
            active = self.hw.nmcli(["-t", "-f", "NAME", "con", "show", "--active"]).stdout.split("\n")
        except (OSError, subprocess.TimeoutExpired):
            return
        state = "off" if radio != "enabled" else "on" if self.cfg["hotspot"] in active else "waiting"
        self.wifi_state = state
        with self.lock:
            self.patterns["wifi"] = {"on": ON, "waiting": blink(0.5, 0.5), "off": OFF}[state]

    # --- Engine status ----------------------------------------------------------
    def set_status(self, state):
        if state == self.state:
            return
        self.state = state
        with self.lock:
            self.patterns["status"] = {"ready": ON, "warnings": blink(2.8, 0.2), "loading": blink(0.5, 0.5)}.get(
                state, blink(0.08, 0.08))
        self.log(f"engine {state}")

    def run_engine(self):
        pid, tick = 0, 0
        since = time.monotonic()
        while True:
            reply = self.engine.call({"cmd": "status"})
            if reply.get("ok"):
                self.engine_state = reply
                if time.monotonic() - self.volume_touched > 1.0 and "volume_db" in reply:
                    self.volume = float(reply["volume_db"])  # changed from the web editor, or a restart
                if self.state not in ("ready", "warnings") or tick % 20 == 0:  # every 10 s: a reload may change them
                    w = self.engine.call({"cmd": "warnings"})
                    self.set_status("warnings" if w.get("active") or self.chain_problem else "ready")
            else:
                self.engine_state = {}
                running = engine_process_running()
                if running != pid:  # started or restarted: loading again
                    pid, since = running, time.monotonic()
                if running and time.monotonic() - since < self.cfg["loading_timeout_s"]:
                    self.set_status("loading")
                else:
                    self.set_status("error")
            if tick % 4 == 0:
                self.update_wifi_led()
            tick += 1
            time.sleep(0.5)

    # --- LEDs and buttons -------------------------------------------------------
    def led_pattern(self, action):
        name, arg = action
        st = self.engine_state
        with self.lock:
            if name in self.patterns:
                return self.patterns[name]
        if name == "preset":
            return ON if st.get("slot") == arg else OFF
        if name == "switch":
            return ON if st.get("switch") else OFF
        if name == "record":
            return blink(0.5, 0.5) if st.get("recording") else OFF
        return OFF

    def run_io(self):
        length = int(self.cfg["chain"]["length"]) if self.chain else 0
        stable = [0xFF] * length      # accepted input bytes (1 = released, buttons to GND)
        candidate = [0xFF] * length
        count = [0] * length
        all_low = 0
        gpio_state = {}
        while True:
            t = time.monotonic()
            for name, led in self.gpio_leds.items():
                level = led_level(self.patterns.get(name, OFF), t)
                if gpio_state.get(name) != level:
                    gpio_state[name] = level
                    led.value = 1 if level else 0
            if self.chain is not None:
                out = bytearray(length)
                for i, action in self.leds.items():
                    if led_level(self.led_pattern(action), t):
                        out[i // 8] |= 1 << (i % 8)
                try:
                    data = self.chain.cycle(bytes(out))
                except OSError as e:
                    self.chain_trouble(f"button chain stopped ({e})")
                    self.chain = None
                    continue
                # Nothing connected to MISO reads as every button pressed: ignore it.
                if all(b == 0 for b in data):
                    all_low += 1
                    if all_low == 250:
                        self.chain_trouble("button chain: every input low - board missing? Buttons ignored")
                    time.sleep(self.SCAN_S)
                    continue
                if all_low >= 250:
                    self.chain_problem = None
                    self.log("button chain: inputs back")
                all_low = 0
                for k in range(length):
                    if data[k] != candidate[k]:
                        candidate[k], count[k] = data[k], 1
                    elif candidate[k] != stable[k]:
                        count[k] += 1
                        if count[k] >= self.DEBOUNCE:
                            pressed = stable[k] & ~candidate[k]  # 1 -> 0 = pressed
                            stable[k] = candidate[k]
                            for bit in range(8):
                                if pressed & (1 << bit):
                                    action = self.buttons.get(8 * k + bit)
                                    if action:
                                        self.actions.put(action)
                time.sleep(self.SCAN_S)
            else:
                time.sleep(0.02)
