"""Ships at sea: predicted offline, realigned whenever the network is there.

Three layers, cheapest first, each one optional except the first:

1. **The prior** (`assets/world/shipping/prior.bin`, `r1/shipping_prior.py`):
   six years of real AIS positions reduced to a 0.1-degree grid -- the mean
   number of ships present, per kind, and the axis of the lane. Shipped with
   the game; one ~50 KB block is read around the player.
2. **The day**: season, weekday, hour and the sea state move that mean. The
   weather is the forecast or archive fetched for the game's date when it was
   online, else the month's climatology of what was fetched, else neutral.
3. **What was seen** (`cache/shipping/learned.sqlite`): every time the game
   is online it fetches the weather and, with an aisstream.io key, listens to
   live AIS around the player for two minutes. Each cell learns a correction
   (ships seen against ships predicted), which way its lane actually runs and
   how fast; ships seen recently enough are put back where they really are.

Everything is approximate by design: a cell is ~11 km, a count is a log byte,
a lane is one axis. The worker writes the ships around the player to
`sea.json`; the game sails them (native/world.cpp) and lets the player board
any of them.

Offline is the normal case, not an error: the sync thread says once, in its
own log (`sea.log`), that it is offline and which base it predicts from.
"""

from __future__ import annotations

import base64
import json
import math
import os
import random
import socket
import sqlite3
import ssl
import struct
import threading
import time
import urllib.parse
import urllib.request
import zlib
from datetime import datetime, timedelta, timezone
from pathlib import Path

from . import harbours

GAME = Path(__file__).resolve().parents[2]
PRIOR = GAME / "assets" / "world" / "shipping" / "prior.bin"
LEARNED = GAME / "cache" / "shipping" / "learned.sqlite"
KEY_FILE = GAME / "cache" / "shipping" / "aisstream.key"

KINDS = ("commercial", "fishing", "passenger", "leisure")
RADIUS = 6000.0            # m around the player
MAX_SHIPS = 40
SLOT = 1800.0              # s of real time a sampled ship keeps its identity
SHRINK = 2.0               # ship-observations before a cell trusts what it saw
LIVE_WINDOW = 3 * 3600.0   # s a seen ship may be dead-reckoned
KNOT = 0.514444

# Prior knowledge per kind: cruising speed (knots), share under way.
SPEED = {"commercial": 13.0, "fishing": 6.0, "passenger": 16.0, "leisure": 6.0}
MOVING = {"commercial": 0.75, "fishing": 0.6, "passenger": 0.8, "leisure": 0.5}
# Hulls per kind (harbours.BOATS names), with weights.
HULLS = {
    "commercial": (("container_a", 3), ("container_b", 3), ("cargo_a", 2), ("cargo_b", 2)),
    "fishing": (("fishing", 1),),
    "passenger": (("ferry", 1),),
    "leisure": (("sail_a", 3), ("sail_b", 3), ("speed_a", 1), ("speed_c", 1), ("speed_e", 1)),
}


# ── the prior ────────────────────────────────────────────────────────────────

class Prior:
    """The shipped base, read one block at a time (four kept)."""

    def __init__(self, path: Path = PRIOR):
        self.path = path
        with open(path, "rb") as f:
            magic = f.read(8)
            if magic[:5] != b"R1SEA":
                raise ValueError(f"{path}: not a sea prior")
            n, = struct.unpack("<I", f.read(4))
            self.meta = json.loads(f.read(n))
            m = self.meta
            self.by = -(-m["rows"] // m["block"])
            self.bx = -(-m["cols"] // m["block"])
            raw = f.read(8 * self.by * self.bx)
            self.index = [struct.unpack_from("<II", raw, 8 * i) for i in range(self.by * self.bx)]
            self.data_at = f.tell()
        self._blocks: dict[int, bytes | None] = {}

    def cell_of(self, lon: float, lat: float) -> tuple[int, int] | None:
        m = self.meta
        row = int((m["lat0"] - lat) / m["cell"])
        col = int(((lon - m["lon0"]) % 360.0) / m["cell"])
        if not (0 <= row < m["rows"] and 0 <= col < m["cols"]):
            return None
        return row, col

    def centre(self, row: int, col: int) -> tuple[float, float]:
        m = self.meta
        return m["lon0"] + (col + 0.5) * m["cell"], m["lat0"] - (row + 0.5) * m["cell"]

    def _block(self, i: int):
        if i not in self._blocks:
            offset, length = self.index[i]
            blob = None
            if length:
                with open(self.path, "rb") as f:
                    f.seek(self.data_at + offset)
                    blob = zlib.decompress(f.read(length))
            if len(self._blocks) >= 4:
                self._blocks.pop(next(iter(self._blocks)))
            self._blocks[i] = blob
        return self._blocks[i]

    def at(self, row: int, col: int):
        """(mean ships per kind, lane bearing or None) of a cell."""
        m, b = self.meta, self.meta["block"]
        blob = self._block((row // b) * self.bx + col // b)
        if blob is None:
            return (0.0,) * len(KINDS), None
        k = (row % b) * b + col % b
        plane = b * b
        ships = tuple(_dequantize(blob[i * plane + k], m) for i in range(len(KINDS)))
        axis = blob[len(KINDS) * plane + k]
        return ships, None if axis == m["noAxis"] else axis * 180.0 / 255.0


def _dequantize(q: int, meta) -> float:
    return 0.0 if q == 0 else meta["floor"] * 2.0 ** ((q - 1) / meta["perOctave"])


# ── the day ──────────────────────────────────────────────────────────────────

def day_factor(kind: str, lon: float, lat: float, when: datetime) -> float:
    """Season, weekday and hour, around a mean of ~1 (the prior is a mean)."""
    doy = when.timetuple().tm_yday
    peak = 196 if lat >= 0 else 15                   # mid-July / mid-January
    tropical = abs(lat) < 23.0
    amplitude = {"leisure": 0.8, "passenger": 0.3, "fishing": 0.1}.get(kind, 0.0)
    if tropical:
        amplitude *= 0.25
    season = 1.0 + amplitude * math.cos(2 * math.pi * (doy - peak) / 365.25)
    hour = (when.hour + when.minute / 60.0 + lon / 15.0) % 24.0     # local solar
    day = 7.0 <= hour < 20.0
    hours = {"leisure": (1.7, 0.25), "passenger": (1.25, 0.6)}.get(kind, (1.0, 1.0))
    diurnal = hours[0] if day else hours[1]
    weekday = 1.0
    if kind == "leisure":
        weekday = 1.35 if when.weekday() >= 5 else 0.86
    return season * diurnal * weekday


def _sea_ease(kind: str, hs: float) -> float:
    if kind == "leisure":
        return math.exp(-(hs / 1.5) ** 2)
    if kind == "fishing":
        return 1.0 / (1.0 + (hs / 3.0) ** 4)
    if kind == "passenger":
        return 1.0 / (1.0 + (hs / 5.0) ** 6)
    return 1.0 / (1.0 + (hs / 10.0) ** 8)


def weather_factor(kind: str, hs: float | None, wind: float | None) -> float:
    """Relative to an ordinary 1 m sea: a gale empties the marinas, not the lanes."""
    if hs is None:
        return 1.0
    f = _sea_ease(kind, hs) / _sea_ease(kind, 1.0)
    if kind == "leisure" and wind is not None and wind > 10.0:
        f *= math.exp(-(wind - 10.0) / 4.0)
    return min(f, 1.5)


# ── what was learned ─────────────────────────────────────────────────────────

class Learned:
    """The local base the online sync grows. Small by construction: one row per
    (cell, kind) ever observed, one per (1-degree cell, day) of weather, and
    the ships seen in the last week."""

    def __init__(self, path: Path = LEARNED):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path, timeout=10)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS cells(cell INTEGER, kind INTEGER, obs REAL, pred REAL,
                vx REAL, vy REAL, moving INTEGER, still INTEGER, knots REAL,
                PRIMARY KEY(cell, kind));
            CREATE TABLE IF NOT EXISTS weather(square INTEGER, day INTEGER, hs REAL, wind REAL,
                PRIMARY KEY(square, day));
            CREATE TABLE IF NOT EXISTS vessels(mmsi INTEGER PRIMARY KEY, kind INTEGER,
                lon REAL, lat REAL, knots REAL, cog REAL, t REAL, length REAL);
            CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
        """)

    def cell(self, cell_id: int, kind: int):
        return self.db.execute("SELECT obs, pred, vx, vy, moving, still, knots FROM cells"
                               " WHERE cell=? AND kind=?", (cell_id, kind)).fetchone()

    def weather(self, lon: float, lat: float, day: int):
        """(hs, wind, source) for a day: that day, else the month's mean here."""
        sq = square(lon, lat)
        row = self.db.execute("SELECT hs, wind FROM weather WHERE square=? AND day=?", (sq, day)).fetchone()
        if row:
            return row[0], row[1], "jour"
        month = (datetime(1970, 1, 1) + timedelta(days=day)).month
        rows = self.db.execute("SELECT day, hs, wind FROM weather WHERE square=?", (sq,)).fetchall()
        same = [(h, w) for d, h, w in rows if (datetime(1970, 1, 1) + timedelta(days=d)).month == month]
        if same:
            return (sum(h for h, _ in same) / len(same), sum(w for _, w in same) / len(same), "climatologie")
        return None, None, "neutre"

    def vessels(self, lon, lat, span, since):
        return self.db.execute(
            "SELECT mmsi, kind, lon, lat, knots, cog, t, length FROM vessels"
            " WHERE lon BETWEEN ? AND ? AND lat BETWEEN ? AND ? AND t >= ?",
            (lon - span, lon + span, lat - span, lat + span, since)).fetchall()

    def meta(self, key, default=None):
        row = self.db.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
        return row[0] if row else default

    def set_meta(self, key, value):
        self.db.execute("INSERT OR REPLACE INTO meta VALUES(?, ?)", (key, str(value)))
        self.db.commit()


def square(lon: float, lat: float) -> int:
    return int(math.floor(lat) + 90) * 360 + int(math.floor(lon) + 180) % 360


def day_number(when: datetime) -> int:
    return int(when.timestamp() // 86400)


# ── the ships ────────────────────────────────────────────────────────────────

def _poisson(lam: float, rng: random.Random) -> int:
    if lam <= 0:
        return 0
    if lam > 30:
        return max(0, int(round(rng.gauss(lam, math.sqrt(lam)))))
    limit, k, p = math.exp(-lam), 0, 1.0
    while True:
        p *= rng.random()
        if p <= limit:
            return k
        k += 1


def _offset(lon, lat, east, north):
    return (lon + east / (111_320.0 * max(0.05, math.cos(math.radians(lat)))), lat + north / 111_320.0)


def _metres(lon0, lat0, lon1, lat1):
    k = math.cos(math.radians((lat0 + lat1) / 2))
    return (lon1 - lon0) * 111_320.0 * k, (lat1 - lat0) * 111_320.0


def _pick(options, rng):
    total = sum(w for _, w in options)
    r = rng.random() * total
    for name, w in options:
        r -= w
        if r <= 0:
            return name
    return options[-1][0]


def plan_ships(prior: Prior, learned: Learned | None, lon: float, lat: float,
               game_unix: float, real_unix: float, radius: float = RADIUS):
    """The ships within `radius` of (lon, lat), and where the numbers came from.

    Deterministic for a given (cell, kind, half-hour slot): asked twice, the
    same ship is at the same place, so the game can match it by id.
    """
    when = datetime.fromtimestamp(game_unix, timezone.utc)
    day = day_number(when)
    hs, wind, weather_source = (learned.weather(lon, lat, day) if learned else (None, None, "neutre"))
    slot = math.floor(real_unix / SLOT)
    since = real_unix - slot * SLOT
    m = prior.meta
    span_lat = radius / 111_320.0 + m["cell"]
    span_lon = radius / (111_320.0 * max(0.05, math.cos(math.radians(lat)))) + m["cell"]
    here = prior.cell_of(lon, lat)
    if here is None:
        return [], {"weather": weather_source}
    r0, c0 = here
    dr, dc = int(span_lat / m["cell"]) + 1, int(span_lon / m["cell"]) + 1
    cols = m["cols"]

    # Ships seen live, dead-reckoned to the game's instant when it is close
    # enough to when they were seen. They stand in for sampled ones.
    live, live_count = [], {}
    if learned and abs(game_unix - real_unix) < LIVE_WINDOW:
        for mmsi, kind, vlon, vlat, knots, cog, t, length in learned.vessels(
                lon, lat, max(span_lat, span_lon), game_unix - LIVE_WINDOW):
            dt = game_unix - t
            moving = knots > 0.8
            speed = knots * KNOT if moving else 0.0
            plon, plat = _offset(vlon, vlat, math.sin(math.radians(cog)) * speed * dt,
                                 math.cos(math.radians(cog)) * speed * dt)
            cell = prior.cell_of(plon, plat)
            if cell is None:
                continue
            live_count[(cell, kind)] = live_count.get((cell, kind), 0) + 1
            live.append((f"ais-{mmsi}", KINDS[kind], plon, plat, cog, speed, length,
                         random.Random(mmsi)))

    ships = []
    for row in range(r0 - dr, r0 + dr + 1):
        if not (0 <= row < m["rows"]):
            continue
        for col in range(c0 - dc, c0 + dc + 1):
            col %= cols
            means, axis = prior.at(row, col)
            if not any(means):
                continue
            clon, clat = prior.centre(row, col)
            cell_id = row * cols + col
            for k, kind in enumerate(KINDS):
                if not means[k]:
                    continue
                lam = means[k] * day_factor(kind, clon, clat, when) * weather_factor(kind, hs, wind)
                seen = learned.cell(cell_id, k) if learned else None
                bearing, knots, moving_share = axis, SPEED[kind], MOVING[kind]
                if seen:
                    obs, pred, vx, vy, moving, still, sum_knots = seen
                    lam *= (obs + SHRINK) / (pred + SHRINK)
                    if moving + still >= 3:
                        moving_share = (moving + MOVING[kind]) / (moving + still + 1)
                    if moving >= 3:
                        knots = sum_knots / moving
                rng = random.Random(hash((cell_id, k, slot)) & 0xFFFFFFFF)
                count = _poisson(lam, rng) - live_count.get(((row, col), k), 0)
                for n in range(max(0, count)):
                    s = random.Random(rng.random())
                    plon = clon + (s.random() - 0.5) * m["cell"]
                    plat = clat + (s.random() - 0.5) * m["cell"]
                    under_way = s.random() < moving_share
                    if bearing is None:
                        heading = s.random() * 360.0
                    else:
                        heading = (bearing + (180.0 if s.random() < 0.5 else 0.0) + s.gauss(0, 6)) % 360.0
                    if seen and seen[4] >= 3:
                        # The lane's real direction, learned: a ship heading
                        # against it turns round, as often as the flow is one-way.
                        flow = math.degrees(math.atan2(seen[2], seen[3])) % 360.0
                        coherence = math.hypot(seen[2], seen[3]) / seen[4]
                        if abs((heading - flow + 180.0) % 360.0 - 180.0) > 90.0 and s.random() < coherence:
                            heading = (heading + 180.0) % 360.0
                    speed = knots * KNOT * (0.8 + 0.4 * s.random()) if under_way else 0.0
                    if speed:
                        plon, plat = _offset(plon, plat, math.sin(math.radians(heading)) * speed * since,
                                             math.cos(math.radians(heading)) * speed * since)
                    ships.append((f"{cell_id}-{k}-{slot}-{n}", kind, plon, plat, heading, speed, None, s))
    ships.extend(live)

    out = []
    for sid, kind, plon, plat, heading, speed, length, s in ships:
        east, north = _metres(lon, lat, plon, plat)
        distance = math.hypot(east, north)
        if distance > radius:
            continue
        hull = harbours.BOATS[_pick(HULLS[kind], s)]
        out.append({"id": sid, "kind": kind, "model": hull.name, "lon": round(plon, 7), "lat": round(plat, 7),
                    "heading": round(heading, 2), "speed": round(speed, 2), "distance": round(distance, 1)})
    # Ships really seen first: a prediction never crowds out a real one.
    out.sort(key=lambda ship: (not ship["id"].startswith("ais-"), ship["distance"]))
    return out[:MAX_SHIPS], {"weather": weather_source, "hs": hs, "wind": wind, "live": len(live)}


def hull_docs(names, game_root: Path = GAME):
    """One scene-node document per hull, which the game clones for every ship
    of that hull (the container ship's boxes included)."""
    from .geodesy import Anchor
    anchor = Anchor.at(0.0, 0.0, 0.0)
    docs, specs = {}, {}
    for name in names:
        kind = harbours.BOATS[name]
        beam = harbours.hull_beam(kind, game_root)
        nodes, manifest = harbours.boat_nodes([harbours.Berth(kind, 0.0, 0.0, (0.0, -1.0), beam)],
                                              anchor, game_root)
        node = nodes[0]
        node["name"] = "Ship " + name
        node["transform"] = {"position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0],
                             "scale": [1.0, 1.0, 1.0]}
        docs[name] = node
        specs[name] = {k: manifest[0][k] for k in ("length", "beam", "top", "accel", "turn")}
    return docs, specs


def write_sea(session: Path, prior: Prior, learned: Learned | None, lon, lat, game_unix, status: dict):
    ships, source = plan_ships(prior, learned, lon, lat, game_unix, time.time())
    docs, specs = hull_docs(sorted({s["model"] for s in ships}))
    for s in ships:
        s.update(specs[s["model"]])
    doc = {"at": time.time(), "lon": lon, "lat": lat, "gameTime": game_unix,
           "ships": ships, "hulls": docs, "source": dict(source, **status)}
    tmp = session / "sea.json.tmp"
    tmp.write_text(json.dumps(doc), encoding="utf-8")
    os.replace(tmp, session / "sea.json")
    return doc


# ── going online ─────────────────────────────────────────────────────────────

def _log(session: Path, *words):
    with open(session / "sea.log", "a", encoding="utf-8") as f:
        f.write(datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ ") + " ".join(map(str, words)) + "\n")


def _get_json(url: str, timeout=15.0):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read())


def fetch_weather(learned: Learned, lon: float, lat: float, game_day: int) -> int:
    """The sea state and wind around the game's date, stored per day.

    Within the forecast horizon the forecast; beyond it, the same days a year
    earlier, which is what a climatology is made of. Returns days stored.
    """
    today = int(time.time() // 86400)
    target = game_day if game_day <= today + 14 else game_day - 365
    start = datetime(1970, 1, 1) + timedelta(days=target - 3)
    end = datetime(1970, 1, 1) + timedelta(days=min(target + 3, today + 14))
    q = {"latitude": f"{lat:.2f}", "longitude": f"{lon:.2f}", "timezone": "GMT",
         "start_date": start.strftime("%Y-%m-%d"), "end_date": end.strftime("%Y-%m-%d")}
    marine = _get_json("https://marine-api.open-meteo.com/v1/marine?" +
                       urllib.parse.urlencode(dict(q, daily="wave_height_max")))
    try:
        wind = _get_json("https://api.open-meteo.com/v1/forecast?" +
                         urllib.parse.urlencode(dict(q, daily="wind_speed_10m_max", wind_speed_unit="ms")))
        winds = dict(zip(wind["daily"]["time"], wind["daily"]["wind_speed_10m_max"]))
    except Exception:
        winds = {}
    sq, stored = square(lon, lat), 0
    for date, hs in zip(marine["daily"]["time"], marine["daily"]["wave_height_max"]):
        if hs is None:
            continue
        d = int(datetime.strptime(date, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp() // 86400)
        learned.db.execute("INSERT OR REPLACE INTO weather VALUES(?, ?, ?, ?)",
                           (sq, d, float(hs), winds.get(date)))
        stored += 1
    learned.db.commit()
    return stored


def _proxied_socket(host: str, port: int, timeout: float) -> socket.socket:
    """A TCP connection that honours HTTPS_PROXY, as urllib does: offline
    tests point it at a dead port, and a raw socket must not slip past."""
    proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy")
    if not proxy:
        return socket.create_connection((host, port), timeout=timeout)
    p = urllib.parse.urlparse(proxy)
    sock = socket.create_connection((p.hostname, p.port or 80), timeout=timeout)
    sock.sendall(f"CONNECT {host}:{port} HTTP/1.1\r\nHost: {host}:{port}\r\n\r\n".encode())
    reply = sock.recv(4096)
    if b" 200" not in reply.split(b"\r\n", 1)[0]:
        sock.close()
        raise OSError("proxy refused CONNECT")
    return sock


class _WebSocket:
    """Just enough of RFC 6455 for one subscription: a text frame out, frames in."""

    def __init__(self, host: str, path: str, timeout: float = 20.0):
        raw = _proxied_socket(host, 443, timeout)
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise OSError("websocket handshake closed")
            head += chunk
        if b" 101" not in head.split(b"\r\n", 1)[0]:
            raise OSError("websocket refused: " + head.split(b"\r\n", 1)[0].decode(errors="replace"))
        self.buffer = head.split(b"\r\n\r\n", 1)[1]

    def _read(self, n):
        while len(self.buffer) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise OSError("websocket closed")
            self.buffer += chunk
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def send(self, text: str, opcode=1):
        payload = text.encode() if isinstance(text, str) else text
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | opcode])
        head += bytes([0x80 | n]) if n < 126 else (bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536
                                                  else bytes([0x80 | 127]) + struct.pack(">Q", n))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def receive(self):
        """The next data message, or None when the server closed."""
        message = b""
        while True:
            b0, b1 = self._read(2)
            opcode, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n, = struct.unpack(">H", self._read(2))
            elif n == 127:
                n, = struct.unpack(">Q", self._read(8))
            payload = self._read(n)
            if opcode == 8:
                return None
            if opcode == 9:
                self.send(payload, opcode=10)
                continue
            if opcode in (0, 1, 2):
                message += payload
                if b0 & 0x80:
                    return message

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def ais_kind(ship_type: int | None, class_b: bool, length: float | None) -> int:
    if ship_type == 30:
        return KINDS.index("fishing")
    if ship_type in (36, 37):
        return KINDS.index("leisure")
    if ship_type is not None and 60 <= ship_type <= 69:
        return KINDS.index("passenger")
    if ship_type is not None and (70 <= ship_type <= 89 or ship_type in (31, 32, 52)):
        return KINDS.index("commercial")
    if length:
        return KINDS.index("commercial") if length > 40 else KINDS.index("leisure")
    return KINDS.index("leisure") if class_b else KINDS.index("commercial")


def listen_ais(key: str, lon: float, lat: float, seconds: float = 120.0, span: float = 0.3):
    """Live positions around (lon, lat) for `seconds`: {mmsi: record}."""
    ws = _WebSocket("stream.aisstream.io", "/v0/stream")
    seen, static = {}, {}
    try:
        ws.send(json.dumps({"APIKey": key,
                            "BoundingBoxes": [[[lat - span, lon - span], [lat + span, lon + span]]],
                            "FilterMessageTypes": ["PositionReport", "StandardClassBPositionReport",
                                                   "ShipStaticData"]}))
        ws.sock.settimeout(10.0)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                raw = ws.receive()
            except socket.timeout:
                continue
            if raw is None:
                break
            msg = json.loads(raw)
            if "error" in msg:
                raise OSError("aisstream: " + str(msg["error"]))
            kind = msg.get("MessageType")
            meta = msg.get("MetaData", {})
            mmsi = meta.get("MMSI")
            body = msg.get("Message", {}).get(kind, {})
            if not mmsi:
                continue
            if kind == "ShipStaticData":
                d = body.get("Dimension") or {}
                static[mmsi] = (body.get("Type"), (d.get("A") or 0) + (d.get("B") or 0) or None)
                continue
            if not body.get("Valid", True):
                continue
            plat, plon = body.get("Latitude"), body.get("Longitude")
            if plat is None or plon is None or abs(plat) > 90 or abs(plon) > 180:
                continue
            seen[mmsi] = {"lon": plon, "lat": plat, "knots": min(float(body.get("Sog") or 0.0), 40.0),
                          "cog": float(body.get("Cog") or 0.0) % 360.0, "t": time.time(),
                          "classB": kind == "StandardClassBPositionReport"}
    finally:
        ws.close()
    for mmsi, record in seen.items():
        ship_type, length = static.get(mmsi, (None, None))
        record["kind"] = ais_kind(ship_type, record["classB"], length)
        record["length"] = length
    return seen


def learn(learned: Learned, prior: Prior, seen: dict, lon, lat, span, when: datetime):
    """Fold one listening window into the base: ships seen against ships
    predicted, per cell and kind, and which way and how fast they went."""
    m = prior.meta
    counts = {}
    for mmsi, r in seen.items():
        cell = prior.cell_of(r["lon"], r["lat"])
        if cell is None:
            continue
        key = (cell[0] * m["cols"] + cell[1], r["kind"])
        c = counts.setdefault(key, [0, 0.0, 0.0, 0, 0, 0.0])
        c[0] += 1
        if r["knots"] > 0.8:
            c[1] += math.sin(math.radians(r["cog"]))
            c[2] += math.cos(math.radians(r["cog"]))
            c[3] += 1
            c[5] += r["knots"]
        else:
            c[4] += 1
        learned.db.execute("INSERT OR REPLACE INTO vessels VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                           (mmsi, r["kind"], r["lon"], r["lat"], r["knots"], r["cog"], r["t"], r["length"]))
    # Every cell of the window was predicted, seen or not.
    r0, c0 = prior.cell_of(lon - span, lat + span) or (0, 0)
    r1, c1 = prior.cell_of(lon + span, lat - span) or (0, 0)
    for row in range(r0 + 1, r1):          # cells fully inside the window only
        for col in range(c0 + 1, c1):
            means, _ = prior.at(row, col)
            clon, clat = prior.centre(row, col)
            for k, kind in enumerate(KINDS):
                pred = means[k] * day_factor(kind, clon, clat, when)
                key = (row * m["cols"] + col, k)
                obs = counts.get(key, [0, 0, 0, 0, 0, 0])
                if not pred and not obs[0]:
                    continue
                learned.db.execute(
                    "INSERT INTO cells VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(cell, kind) DO UPDATE SET"
                    " obs=obs+excluded.obs, pred=pred+excluded.pred, vx=vx+excluded.vx, vy=vy+excluded.vy,"
                    " moving=moving+excluded.moving, still=still+excluded.still, knots=knots+excluded.knots",
                    (key[0], k, obs[0], pred, obs[1], obs[2], obs[3], obs[4], obs[5]))
    learned.db.execute("DELETE FROM vessels WHERE t < ?", (time.time() - 7 * 86400,))
    learned.db.commit()


class Sea:
    """The worker's sea: plans ships on request, syncs when it can."""

    PLAN_EVERY = 5.0
    SYNC_EVERY = 900.0
    RETRY_OFFLINE = 600.0

    def __init__(self, session: Path):
        self.session = session
        self.prior = Prior() if PRIOR.exists() else None
        self.status = {"online": False, "synced": None}
        self.stop = threading.Event()
        self.threads: list[threading.Thread] = []
        self.where = None
        if self.prior is None:
            _log(session, "no sea prior at", PRIOR, "- no ships at sea")

    def start(self):
        if self.prior is None:
            return
        threads = [
            threading.Thread(target=self._plan_loop, daemon=True, name="sea-plan"),
            threading.Thread(target=self._sync_loop, daemon=True, name="sea-sync"),
        ]
        for thread in threads:
            thread.start()
            self.threads.append(thread)

    def close(self):
        self.stop.set()
        for thread in self.threads:
            thread.join(timeout=2.0)

    def _request(self):
        try:
            doc = json.loads((self.session / "sea_request.json").read_text(encoding="utf-8"))
            return float(doc["lon"]), float(doc["lat"]), float(doc["time"])
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def _plan_loop(self):
        learned = Learned()
        self.status["synced"] = learned.meta("synced")
        while not self.stop.wait(self.PLAN_EVERY):
            req = self._request()
            if req is None:
                continue
            self.where = req
            try:
                write_sea(self.session, self.prior, learned, *req, dict(self.status))
            except Exception as error:
                _log(self.session, "planning failed:", repr(error))

    def _sync_loop(self):
        learned = Learned()
        said_offline = said_no_key = False
        while not self.stop.wait(5.0):
            if self.where is None:
                continue
            lon, lat, game_unix = self.where
            try:
                days = fetch_weather(learned, lon, lat, day_number(datetime.fromtimestamp(game_unix, timezone.utc)))
                key = (os.environ.get("R1WORLD_AISSTREAM_KEY") or
                       (KEY_FILE.read_text(encoding="utf-8").strip() if KEY_FILE.exists() else ""))
                ships = 0
                if key:
                    seen = listen_ais(key, lon, lat)
                    learn(learned, self.prior, seen, lon, lat, 0.3, datetime.now(timezone.utc))
                    ships = len(seen)
                elif not said_no_key:
                    _log(self.session, "no aisstream.io key (", KEY_FILE, "or R1WORLD_AISSTREAM_KEY ):"
                         " weather only, no live ships")
                    said_no_key = True
                now = datetime.now(timezone.utc).isoformat(timespec="seconds")
                learned.set_meta("synced", now)
                self.status.update(online=True, synced=now)
                _log(self.session, f"synced: {days} days of weather, {ships} live ships near {lon:.3f}, {lat:.3f}")
                said_offline = False
                self.stop.wait(self.SYNC_EVERY)
            except Exception as error:
                self.status["online"] = False
                if not said_offline:
                    _log(self.session, "offline (", type(error).__name__, error, ") - predicting from the local"
                         " base, last synced", learned.meta("synced", "never"))
                    said_offline = True
                self.stop.wait(self.RETRY_OFFLINE)
