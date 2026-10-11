#!/usr/bin/env python3
"""mbx-control: web editor of MusicalBOX SM, for the phone.

Serves the page in control/web/ and a small JSON API. Talks to mbx-engine
through its control socket (engine.json "control_socket"), reads and writes
config/programs.json and config/presets.json.

Only the Python standard library: nothing to install.

    python3 control/mbx_control.py                 # port 8080, config/ next to control/
    python3 control/mbx_control.py --port 80       # as a service (see system/)
"""

import argparse
import datetime
import json
import os
import pathlib
import shutil
import contextlib
import socket
import subprocess
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEEP_BACKUPS = 30


# ---------------------------------------------------------------------------
# JSON files with comments
# ---------------------------------------------------------------------------

def strip_comments(text):
    """Removes // and /* */ comments outside strings (the engine accepts them)."""
    out = []
    i, n = 0, len(text)
    in_string = False
    while i < n:
        c = text[i]
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
        elif c == '"':
            in_string = True
            out.append(c)
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return json.loads(strip_comments(text)), ("//" in text or "/*" in text)


def write_json_atomic(path, data):
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(format_json(data))
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def format_json(data, indent=0):
    """Readable JSON: objects one key per line, short arrays and small
    objects (like an effect's params) on one line."""
    pad = "  " * indent
    inner = "  " * (indent + 1)
    one_line = json.dumps(data, ensure_ascii=False)
    if not isinstance(data, (dict, list)):
        return one_line
    simple = all(not isinstance(v, (dict, list)) for v in (data.values() if isinstance(data, dict) else data))
    if len(one_line) <= 90 and (simple or indent >= 4):
        return one_line
    if isinstance(data, dict):
        if not data:
            return "{}"
        items = [f"{inner}{json.dumps(k, ensure_ascii=False)}: {format_json(v, indent + 1)}" for k, v in data.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if not data:
        return "[]"
    items = [inner + format_json(v, indent + 1) for v in data]
    return "[\n" + ",\n".join(items) + "\n" + pad + "]"


# ---------------------------------------------------------------------------
# Engine control socket
# ---------------------------------------------------------------------------

class Engine:
    """One connection to the engine, reopened when needed; one request at a time."""

    def __init__(self, path):
        self.path = path
        self.sock = None
        self.buffer = b""
        self.lock = threading.Lock()

    def _connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect(self.path)
        self.sock = s
        self.buffer = b""

    def call(self, request):
        with self.lock:
            for attempt in (1, 2):
                try:
                    if self.sock is None:
                        self._connect()
                    self.sock.sendall(json.dumps(request).encode() + b"\n")
                    while b"\n" not in self.buffer:
                        chunk = self.sock.recv(65536)
                        if not chunk:
                            raise ConnectionError("engine closed the connection")
                        self.buffer += chunk
                    line, self.buffer = self.buffer.split(b"\n", 1)
                    return json.loads(line)
                except (OSError, ConnectionError, ValueError) as e:
                    if self.sock:
                        self.sock.close()
                    self.sock = None
                    if attempt == 2:
                        return {"ok": False, "error": f"engine not reachable ({e})", "offline": True}
        return {"ok": False, "error": "engine not reachable", "offline": True}


# ---------------------------------------------------------------------------
# Read-only system: unlocked only for the time of a save
# ---------------------------------------------------------------------------

def mount_of(path):
    """(mount point, read-only?) of the filesystem holding `path`."""
    path = os.path.realpath(path)
    best, ro = "/", False
    try:
        with open("/proc/mounts", encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 4:
                    continue
                mp = parts[1].replace("\\040", " ")
                if (path == mp or path.startswith(mp.rstrip("/") + "/")) and len(mp) >= len(best):
                    best, ro = mp, "ro" in parts[3].split(",")
    except OSError:
        pass
    return best, ro


@contextlib.contextmanager
def writable(path):
    """If the disk is read-only (system/mbx-readonly.sh), makes it writable
    for the block, then forces the data to the card and locks it again."""
    mp, ro = mount_of(path)
    if not ro:
        yield
        return
    r = subprocess.run(["sudo", "-n", "/usr/bin/mount", "-o", "remount,rw", mp], capture_output=True, text=True)
    if r.returncode != 0:
        raise OSError(f"cannot unlock {mp} for writing: {r.stderr.strip() or 'sudo refused'}")
    try:
        yield
    finally:
        os.sync()
        for _ in range(10):  # a file still open for writing makes it "busy" for a moment
            if subprocess.run(["sudo", "-n", "/usr/bin/mount", "-o", "remount,ro", mp], capture_output=True).returncode == 0:
                break
            time.sleep(0.3)
        else:
            print(f"mbx-control: warning: {mp} left writable (busy)", flush=True)


# ---------------------------------------------------------------------------
# Configuration files
# ---------------------------------------------------------------------------

class Config:
    def __init__(self, folder):
        self.folder = pathlib.Path(folder)
        self.lock = threading.Lock()

    def path(self, name):
        return self.folder / name

    def load(self):
        result = {"had_comments": []}
        for key in ("programs", "presets", "instruments", "engine"):
            path = self.path(f"{key}.json")
            try:
                data, comments = read_json(path)
                result[key] = data
                if comments and key in ("programs", "presets"):
                    result["had_comments"].append(path.name)
            except FileNotFoundError:
                result[key] = None
            except ValueError as e:
                result[key] = None
                result.setdefault("errors", []).append(f"{path.name}: {e}")
        result["schema"] = {}
        for key in ("programs", "presets"):
            try:
                result["schema"][key] = json.loads(self.path(f"schema/{key}.schema.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                result["schema"][key] = None
        return result

    def backup(self):
        folder = self.folder / "backup"
        folder.mkdir(exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        for name in ("programs.json", "presets.json"):
            src = self.path(name)
            if src.exists():
                shutil.copy2(src, folder / f"{stamp}-{name}")
        stamps = sorted({p.name[:15] for p in folder.glob("*-programs.json")})
        for old in stamps[:-KEEP_BACKUPS]:
            for p in folder.glob(old + "-*"):
                p.unlink()
        return stamp

    def backups(self):
        folder = self.folder / "backup"
        if not folder.exists():
            return []
        return sorted({p.name[:15] for p in folder.glob("*-programs.json")}, reverse=True)

    def save(self, programs, presets):
        with self.lock, writable(self.folder):
            stamp = self.backup()
            write_json_atomic(self.path("programs.json"), programs)
            write_json_atomic(self.path("presets.json"), presets)
            return stamp

    def restore(self, stamp):
        folder = self.folder / "backup"
        with self.lock:
            files = {name: folder / f"{stamp}-{name}" for name in ("programs.json", "presets.json")}
            if not all(p.exists() for p in files.values()):
                raise FileNotFoundError(stamp)
            with writable(self.folder):
                self.backup()
                for name, src in files.items():
                    shutil.copy2(src, self.path(name))


def cpu_temperature():
    try:
        return round(int(pathlib.Path("/sys/class/thermal/thermal_zone0/temp").read_text()) / 1000.0, 1)
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

PASS_THROUGH = {"preset", "switch", "volume", "panic", "note", "record"}
CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".png": "image/png",
                 ".json": "application/json", ".webmanifest": "application/manifest+json"}


def recordings_in(folder):
    """Recordings made by the engine (mbx-rec-*.wav), newest first."""
    out = []
    for p in sorted(pathlib.Path(folder).glob("mbx-rec-*.wav"), reverse=True):
        try:
            size = p.stat().st_size
        except OSError:
            continue
        out.append({"name": p.name, "mb": round(size / 1048576, 1), "seconds": round(max(0, size - 44) / (48000 * 4), 1)})
    return out


def make_handler(engine, config, web_root, record_folder):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "mbx-control"

        def log_message(self, fmt, *args):  # quiet: only errors
            pass

        def reply(self, data, status=HTTPStatus.OK):
            body = json.dumps(data, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def body(self):
            length = int(self.headers.get("Content-Length") or 0)
            if length <= 0:
                return {}
            return json.loads(self.rfile.read(length).decode("utf-8"))

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/api/state":
                state = config.load()
                state["bank"] = engine.call({"cmd": "bank"})
                state["instruments_loaded"] = engine.call({"cmd": "instruments"})
                state["status"] = engine.call({"cmd": "status"})
                state["backups"] = config.backups()
                return self.reply(state)
            if path == "/api/status":
                status = engine.call({"cmd": "status"})
                status["temperature"] = cpu_temperature()
                return self.reply(status)
            if path == "/api/recordings":
                return self.reply({"ok": True, "folder": record_folder, "files": recordings_in(record_folder)})
            if path.startswith("/api/recordings/"):
                return self.download(path.rsplit("/", 1)[1])
            if path == "/api/warnings":
                return self.reply(engine.call({"cmd": "warnings"}))
            return self.static(path)

        def do_POST(self):
            path = self.path.split("?", 1)[0]
            try:
                req = self.body()
            except ValueError:
                return self.reply({"ok": False, "error": "request is not JSON"}, HTTPStatus.BAD_REQUEST)

            if path == "/api/cmd":
                if req.get("cmd") not in PASS_THROUGH:
                    return self.reply({"ok": False, "error": "command not allowed"}, HTTPStatus.BAD_REQUEST)
                return self.reply(engine.call(req))
            if path == "/api/preview":
                return self.reply(engine.call({"cmd": "preview", "programs": req.get("programs"),
                                               "presets": req.get("presets")}))
            if path == "/api/save":
                programs, presets = req.get("programs"), req.get("presets")
                check = engine.call({"cmd": "check", "programs": programs, "presets": presets})
                if not check.get("ok"):
                    if check.get("offline"):
                        return self.reply(check)
                    return self.reply({"ok": False, "error": check.get("error") or "configuration not usable",
                                       "warnings": check.get("warnings", [])})
                try:
                    stamp = config.save(programs, presets)
                except OSError as e:
                    return self.reply({"ok": False, "error": f"file non scritti: {e}"})
                reload = engine.call({"cmd": "reload"})
                return self.reply({"ok": reload.get("ok", False), "backup": stamp,
                                   "warnings": reload.get("warnings", check.get("warnings", [])),
                                   "error": reload.get("error")})
            if path == "/api/recordings/delete":
                target = self.recording(str(req.get("name", "")))
                if not target:
                    return self.reply({"ok": False, "error": "recording not found"})
                target.unlink()
                return self.reply({"ok": True})
            if path == "/api/revert":
                return self.reply(engine.call({"cmd": "reload"}))
            if path == "/api/restore":
                try:
                    config.restore(str(req.get("backup", "")))
                except FileNotFoundError:
                    return self.reply({"ok": False, "error": "backup not found"})
                except OSError as e:
                    return self.reply({"ok": False, "error": f"file non scritti: {e}"})
                return self.reply(engine.call({"cmd": "reload"}))
            return self.reply({"ok": False, "error": "not found"}, HTTPStatus.NOT_FOUND)

        def recording(self, name):
            """Path of a recording by name; only mbx-rec-*.wav inside the folder."""
            if not name.startswith("mbx-rec-") or not name.endswith(".wav") or "/" in name or ".." in name:
                return None
            target = pathlib.Path(record_folder) / name
            return target if target.is_file() else None

        def download(self, name):
            target = self.recording(name)
            if not target:
                return self.reply({"ok": False, "error": "recording not found"}, HTTPStatus.NOT_FOUND)
            size = target.stat().st_size
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.end_headers()
            with open(target, "rb") as f:
                shutil.copyfileobj(f, self.wfile, 1 << 16)

        def static(self, path):
            if path in ("", "/"):
                path = "/index.html"
            target = (web_root / path.lstrip("/")).resolve()
            if web_root not in target.parents or not target.is_file():
                return self.reply({"ok": False, "error": "not found"}, HTTPStatus.NOT_FOUND)
            data = target.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", CONTENT_TYPES.get(target.suffix, "application/octet-stream"))
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

    return Handler


def main():
    ap = argparse.ArgumentParser(description="MusicalBOX SM web editor")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--config", default=str(ROOT / "config"), help="folder with programs.json, presets.json...")
    ap.add_argument("--socket", default=None, help="engine control socket (default: from engine.json)")
    ap.add_argument("--no-panel", action="store_true", help="do not use the GPIO front panel")
    args = ap.parse_args()

    config = Config(args.config)
    try:
        engine_json = read_json(config.path("engine.json"))[0] or {}
    except (OSError, ValueError):
        engine_json = {}
    socket_path = args.socket or engine_json.get("control_socket", "/tmp/mbx-engine.sock")
    engine = Engine(socket_path)
    if not args.no_panel:
        from panel import Panel  # noqa: PLC0415
        Panel(Engine(socket_path), engine_json.get("panel"))  # own connection: buttons never wait for the web
    web_root = (pathlib.Path(__file__).resolve().parent / "web").resolve()

    record_folder = engine_json.get("record_folder") or "/tmp"
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(engine, config, web_root, record_folder))
    server.daemon_threads = True
    print(f"mbx-control: http://{args.bind}:{args.port}/  config {config.folder}  engine {socket_path}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
