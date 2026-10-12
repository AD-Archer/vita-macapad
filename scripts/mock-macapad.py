#!/usr/bin/env python3
"""A pretend Macapad for trying the app in Vita3K without pairing with your real Mac.

It answers like Macapad does: health, pairing (approved at once), status with a song that plays
along, weather and stats, artwork, the three catalogs, app and website icons, and /api/run (which
it prints). Any token is accepted.

Live input (the controller and the typing screen) gets a session from /api/input/session; the UDP
packets that follow, on the same port number, are printed when what's held changes or the pointer
moves, and answered like Macapad answers them.

  python3 scripts/mock-macapad.py            listens on 127.0.0.1:7399
  python3 scripts/mock-macapad.py --port 7317 --host 0.0.0.0   (to be found by a real Vita's scan)
  python3 scripts/mock-macapad.py --old      a Macapad from before 1.0: no live input, no switches
  python3 scripts/mock-macapad.py --paused   paused on the Mac (423s, and "off" for live input)
"""
import argparse
import json
import secrets
import socket
import threading
import time
import zlib
import struct
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MAC = {"id": "mock-mac-0001", "name": "Mock MacBook Pro"}
STARTED = time.time()
SONG_MS = 243_000

ACTIONS = [
    ("play-pause", "Play/Pause", "media", "play_pause"), ("next-track", "Next", "media", "skip_next"),
    ("previous-track", "Previous", "media", "skip_previous"), ("volume-up", "Volume Up", "media", "volume_up"),
    ("volume-down", "Volume Down", "media", "volume_down"), ("mute", "Mute", "media", "volume_off"),
    ("emoji", "Emoji & Symbols", "system", "mood"), ("spotlight", "Spotlight", "system", "search"),
    ("screenshot-ui", "Screenshot", "system", "photo_camera"), ("mission-control", "Mission Control", "system", "grid_view"),
    ("lock-screen", "Lock Screen", "system", "lock"), ("minimize", "Minimize", "windows", "arrow_downward"),
    ("fullscreen", "Full Screen", "windows", "fullscreen"), ("quit-app", "Quit App", "windows", "power_settings_new"),
    ("reload", "Reload", "browser", "refresh"), ("new-tab", "New Tab", "browser", "tab"),
    ("copy", "Copy", "editing", "content_copy"), ("paste", "Paste", "editing", "content_paste"),
    ("window-left", "Left half", "windows", "splitscreen_left"), ("window-right", "Right half", "windows", "splitscreen_right"),
    ("window-top-left", "Top-left quarter", "windows", "north_west"), ("window-maximize", "Fill screen", "windows", "fit_screen"),
    ("window-center", "Center", "windows", "filter_center_focus"), ("window-restore", "Restore size", "windows", "restore"),
    ("keep-awake", "Keep Mac awake", "system", "coffee"), ("jiggler", "Mouse jiggler", "system", "mouse"),
    ("siri", "Siri", "system", "mic"), ("brightness-up", "Brightness up", "system", "brightness_high"),
    ("brightness-down", "Brightness down", "system", "brightness_low"), ("dark-mode", "Dark mode", "system", "dark_mode"),
    ("sleep", "Sleep", "system", "bedtime"), ("force-quit", "Force Quit", "system", "cancel"),
    ("voiceover", "VoiceOver", "accessibility", "record_voice_over"), ("zoom-in", "Zoom in", "accessibility", "zoom_in"),
    ("zoom-out", "Zoom out", "accessibility", "zoom_out"), ("invert-colors", "Invert colors", "accessibility", "invert_colors"),
    ("speak-selection", "Speak selection", "accessibility", "text_to_speech"),
    ("left-click", "Left click", "mouse", "left_click"), ("right-click", "Right click", "mouse", "right_click"),
    ("deploy", "Deploy", "custom", "terminal"),
]
APPS = [("Safari", "com.apple.Safari"), ("Spotify", "com.spotify.client"), ("Visual Studio Code", "com.microsoft.VSCode"),
        ("Terminal", "com.apple.Terminal"), ("Messages", "com.apple.MobileSMS"), ("Notes", "com.apple.Notes")]
SHORTCUTS = ["Focus Mode", "Start Timer", "Send Location", "居家模式"]


def png(w, h, pixel):
    """A PNG from pixel(x, y) -> (r, g, b, a), with nothing but the standard library."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pixel(x, y))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def rounded_icon(size, color):
    r = size * 0.22
    def px(x, y):
        dx = max(r - x, 0, x - (size - 1 - r))
        dy = max(r - y, 0, y - (size - 1 - r))
        inside = dx * dx + dy * dy <= r * r
        shade = 1 - 0.25 * y / size
        return (int(color[0] * shade), int(color[1] * shade), int(color[2] * shade), 255 if inside else 0)
    return png(size, size, px)


def artwork(size):
    return png(size, size, lambda x, y: (40 + x * 200 // size, 30 + y * 120 // size, 160 - x * 100 // size, 255))


ICON_COLORS = [(10, 132, 255), (48, 209, 88), (255, 159, 10), (191, 90, 242), (255, 55, 95), (100, 210, 255)]

OPTS = argparse.Namespace(old=False, paused=False, port=7399)
OFF = {"reason": "paused", "message": "Macapad is paused until 3:00 PM."}
SWITCHES = {"keepAwake": False, "jiggler": False}
SESSIONS = {}  # session -> when it was last heard from


def run_steps(steps):
    for step in steps or []:
        kind = step.get("type")
        if kind in SWITCHES:
            SWITCHES[kind] = step["on"] if "on" in step else not SWITCHES[kind]
            print(f"  {kind} is now {'on' if SWITCHES[kind] else 'off'}", flush=True)


def udp_server(host, port):
    """The live input side: checks the session, prints what changes, answers "ack"s."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    last = {}  # client address -> (keys, buttons) last printed
    newest = {}  # session -> the highest "n" taken
    while True:
        data, addr = sock.recvfrom(2048)
        try:
            state = json.loads(data)
        except ValueError:
            print(f"udp {addr}: not JSON: {data[:80]!r}", flush=True)
            continue
        session = state.get("s", "")
        heard = SESSIONS.get(session)
        if OPTS.paused:
            sock.sendto(json.dumps({"type": "error", "reason": "off", "message": OFF["message"]}).encode(), addr)
            continue
        if heard is None or time.time() - heard > 30:
            print(f"udp {addr}: unknown session {session[:8]}…", flush=True)
            sock.sendto(json.dumps({"type": "error", "reason": "session", "message": "Unknown session"}).encode(), addr)
            continue
        SESSIONS[session] = time.time()
        n = state.get("n", 0)
        if n <= newest.get(session, -1):
            print(f"udp {addr}: old packet {n}, ignored", flush=True)
            continue
        newest[session] = n
        held = (tuple(state.get("keys", [])), tuple(state.get("buttons", [])))
        moved = {k: state[k] for k in ("dx", "dy", "sx", "sy") if state.get(k)}
        if held != last.get(addr) or moved:
            keys = "+".join(held[0]) or "-"
            buttons = ",".join(held[1]) or "-"
            print(f"udp n={n} keys={keys} buttons={buttons}" + (f" {moved}" if moved else ""), flush=True)
            last[addr] = held
        if state.get("ack"):
            sock.sendto(b'{"type":"ok"}', addr)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive, like Macapad

    def log_message(self, fmt, *args):
        print(f"{self.command} {self.path} -> {args[1] if len(args) > 1 else ''}", flush=True)

    def send(self, status, body=b"", kind="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(n) or b"{}") if n else {}

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        size = max(16, min(512, int(q.get("size", 128))))
        p = url.path
        if p == "/api/health":
            return self.send(200, {"ok": True, "name": "macapad", "version": "0.4.0", "network": "lan", "mac": MAC, "pairing": True})
        if p.startswith("/api/pair/"):
            return self.send(200, {"status": "approved", "token": "mock-token", "device": {"id": "d1", "name": "PS Vita"}})
        if p == "/api/status":
            elapsed = int((time.time() - STARTED) * 1000)
            track = elapsed // SONG_MS
            return self.send(200, {
                "time": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "utcOffset": -time.timezone if not time.daylight else -time.altzone,
                "mac": MAC, "frontApp": "Visual Studio Code",
                "nowPlaying": {"app": "Spotify", "bundleId": "com.spotify.client", "playing": True,
                               "title": ["Midnight City", "夜に駆ける", "Strobe"][track % 3],
                               "artist": ["M83", "YOASOBI", "deadmau5"][track % 3],
                               "album": "Hurry Up, We're Dreaming", "duration": SONG_MS,
                               "position": elapsed % SONG_MS, "artworkId": f"art{track % 3}"},
                "weather": {"temp": 68, "high": 72, "low": 55, "unit": "F", "symbol": "partly-cloudy",
                            "condition": "Partly Cloudy", "isDay": True, "place": "Burlington, VT"},
                "stats": {"cpu": 23, "memoryUsed": 11.2e9, "memoryTotal": 16e9, "battery": 76, "charging": True,
                          "pluggedIn": True, "diskFree": 182.4e9, "diskTotal": 494e9, "uptime": 389_000, "thermal": "nominal"},
                **({} if OPTS.old else {"keepAwake": {"on": SWITCHES["keepAwake"]}, "jiggler": {"on": SWITCHES["jiggler"]}}),
                **({"off": OFF} if OPTS.paused else {}),
            })
        if p == "/api/status/artwork":
            return self.send(200, artwork(size), "image/png")
        if p == "/api/actions":
            return self.send(200, {"actions": [{"id": i, "name": n, "category": c, "icon": ic, "custom": c == "custom", "steps": []}
                                               for i, n, c, ic in ACTIONS]})
        if p == "/api/apps":
            return self.send(200, {"apps": [{"name": n, "bundleId": b, "path": f"/Applications/{n}.app"} for n, b in APPS]})
        if p == "/api/apps/icon":
            color = ICON_COLORS[sum(map(ord, q.get("bundleId", ""))) % len(ICON_COLORS)]
            return self.send(200, rounded_icon(size, color), "image/png")
        if p == "/api/web/icon":
            host = q.get("host", "")
            if host.endswith(".invalid"):
                return self.send(204)
            color = ICON_COLORS[sum(map(ord, host)) % len(ICON_COLORS)]
            return self.send(200, png(size, size, lambda x, y: (*color, 255)), "image/png")
        if p == "/api/shortcuts":
            return self.send(200, {"shortcuts": SHORTCUTS})
        if p == "/api/me":
            return self.send(200, {"owner": False, "device": {"id": "d1"}, "mac": MAC})
        self.send(404, {"error": {"message": "Not found"}})

    def do_POST(self):
        p = urlparse(self.path).path
        data = self.body()
        if p == "/api/pair":
            return self.send(200, {"id": "pair-1", "code": "4821", "expiresIn": 120, "mac": MAC})
        if p == "/api/pair/qr":
            return self.send(200, {"status": "approved", "token": "mock-token", "device": {"id": "d1"}, "mac": MAC})
        if p == "/api/run":
            print("  run:", json.dumps(data.get("steps"), ensure_ascii=False), flush=True)
            if OPTS.paused:
                return self.send(423, {"error": {"message": OFF["message"]}, "off": OFF})
            run_steps(data.get("steps"))
            return self.send(200, {"ok": True})
        if p == "/api/input/session" and not OPTS.old:
            if OPTS.paused:
                return self.send(423, {"error": {"message": OFF["message"]}, "off": OFF})
            session = secrets.token_hex(16)
            SESSIONS[session] = time.time()
            print(f"  input session {session[:8]}…", flush=True)
            return self.send(200, {"session": session, "udpPort": OPTS.port, "expiresAfter": 30})
        self.send(404, {"error": {"message": "Not found"}})


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7399)
    ap.add_argument("--old", action="store_true", help="act like a Macapad from before 1.0")
    ap.add_argument("--paused", action="store_true", help="act like Macapad is paused on the Mac")
    args = ap.parse_args()
    OPTS.old, OPTS.paused, OPTS.port = args.old, args.paused, args.port
    print(f"pretend Macapad on http://{args.host}:{args.port} (live input on UDP {args.port})", flush=True)
    threading.Thread(target=udp_server, args=(args.host, args.port), daemon=True).start()
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
