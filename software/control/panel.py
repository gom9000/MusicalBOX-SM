"""Front panel of MusicalBOX SM: Panic and effect-switch buttons, Wi-Fi
switch, Wi-Fi LED and status LED, on the Pi's GPIO (gpiozero).

Pins and options come from the "panel" object of engine.json (see README).
Without gpiozero, or on a PC, the panel is simply off.

Status LED:  slow blink = engine loading,  steady = ready,
             steady with a short dark blip every 3 s = ready, with warnings
             (an instrument not loaded, a program skipped, an empty button...),
             fast blink = engine not running / not answering.
Wi-Fi LED:   on = own network active, slow blink = radio on but network
             not (yet) up, off = radio off.
Wi-Fi switch: closed (to GND) = Wi-Fi OFF, open = Wi-Fi on. Not wired = on.
"""

import os
import subprocess
import threading
import time

DEFAULTS = {
    "enabled": True,
    "panic": 22,          # button to GND
    "fx_switch": 23,      # footswitch (momentary) to GND
    "wifi_switch": 5,     # toggle to GND: closed = Wi-Fi off
    "wifi_led": 6,
    "status_led": 24,
    "hotspot": "MusicalBOX-SM",  # NetworkManager connection of the Pi's own network
    "loading_timeout_s": 180,    # engine running this long without answering = error
}

NMCLI = "/usr/bin/nmcli"


def engine_process_running():
    """True if an mbx-engine process exists (started by systemd or by hand)."""
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


class Panel:
    def __init__(self, engine, settings):
        self.engine = engine
        self.cfg = dict(DEFAULTS)
        self.cfg.update({k: v for k, v in (settings or {}).items() if not k.startswith("_")})
        self.ok = False
        self.state = None
        self.wifi_state = None
        if not self.cfg.get("enabled", True):
            print("mbx-control: front panel disabled in engine.json", flush=True)
            return
        try:
            from gpiozero import LED, Button  # noqa: PLC0415 (only on the Pi)
        except ImportError:
            print("mbx-control: gpiozero not available - no front panel (sudo apt install python3-gpiozero)", flush=True)
            return
        try:
            self.status_led = LED(self.cfg["status_led"])
            self.wifi_led = LED(self.cfg["wifi_led"])
            self.panic = Button(self.cfg["panic"], pull_up=True, bounce_time=0.03)
            self.fx = Button(self.cfg["fx_switch"], pull_up=True, bounce_time=0.03)
            self.wifi = Button(self.cfg["wifi_switch"], pull_up=True, bounce_time=0.1)
        except Exception as e:  # not a Pi, pin busy...
            print(f"mbx-control: front panel not available ({e})", flush=True)
            return
        self.panic.when_pressed = lambda: self.engine.call({"cmd": "panic"})
        self.fx.when_pressed = lambda: self.engine.call({"cmd": "switch"})
        self.wifi.when_pressed = lambda: self.set_wifi(False)
        self.wifi.when_released = lambda: self.set_wifi(True)
        self.ok = True
        self.status_led.blink(0.5, 0.5)
        self.set_wifi(not self.wifi.is_pressed)
        threading.Thread(target=self.run, daemon=True).start()
        print("mbx-control: front panel on GPIO " + ", ".join(
            f"{k} {self.cfg[k]}" for k in ("panic", "fx_switch", "wifi_switch", "wifi_led", "status_led")), flush=True)

    # --- Wi-Fi -------------------------------------------------------------
    def set_wifi(self, on):
        try:
            r = subprocess.run(["sudo", "-n", NMCLI, "radio", "wifi", "on" if on else "off"],
                               capture_output=True, text=True, timeout=10)
            if r.returncode != 0:
                print(f"mbx-control: Wi-Fi {'on' if on else 'off'} failed: {r.stderr.strip()}", flush=True)
        except (OSError, subprocess.TimeoutExpired) as e:
            print(f"mbx-control: Wi-Fi {'on' if on else 'off'} failed: {e}", flush=True)
        self.update_wifi_led()

    def update_wifi_led(self):
        try:
            radio = subprocess.run([NMCLI, "radio", "wifi"], capture_output=True, text=True, timeout=3).stdout.strip()
            active = subprocess.run([NMCLI, "-t", "-f", "NAME", "con", "show", "--active"],
                                    capture_output=True, text=True, timeout=3).stdout.split("\n")
        except (OSError, subprocess.TimeoutExpired):
            return
        state = "off" if radio != "enabled" else "on" if self.cfg["hotspot"] in active else "waiting"
        if state != self.wifi_state:
            self.wifi_state = state
            if state == "on":
                self.wifi_led.on()
            elif state == "waiting":
                self.wifi_led.blink(0.5, 0.5)
            else:
                self.wifi_led.off()

    # --- Status ------------------------------------------------------------
    def set_status(self, state):
        if state == self.state:
            return
        self.state = state
        if state == "ready":
            self.status_led.on()
        elif state == "warnings":
            self.status_led.blink(2.8, 0.2)
        elif state == "loading":
            self.status_led.blink(0.5, 0.5)
        else:
            self.status_led.blink(0.08, 0.08)
        print(f"mbx-control: engine {state}", flush=True)

    def run(self):
        pid, since = 0, time.monotonic()
        tick = 0
        while True:
            reply = self.engine.call({"cmd": "status"})
            if reply.get("ok"):
                if self.state not in ("ready", "warnings") or tick % 20 == 0:  # every 10 s: a reload may change them
                    w = self.engine.call({"cmd": "warnings"})
                    self.set_status("warnings" if w.get("active") else "ready")
            else:
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
