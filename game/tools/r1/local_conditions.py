"""Local civil time and current sky conditions, fetched without delaying tiles.

The clock and Sun work offline. Forecasts are optional and cached per ~10 km
cell; a stale forecast is never presented as current weather.
"""
from __future__ import annotations

import json
import math
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
CACHE = GAME / "cache" / "conditions"
REFRESH_SECONDS = 15 * 60
CURRENT_SECONDS = 2 * 60 * 60


def _cell(lon: float, lat: float) -> tuple[float, float]:
    return round(lon, 1), round(lat, 1)


def _cache_path(cell: tuple[float, float]) -> Path:
    return CACHE / f"{cell[1]:+.1f}_{cell[0]:+.1f}.json"


def _forecast(lon: float, lat: float) -> dict:
    query = urllib.parse.urlencode({
        "latitude": f"{lat:.3f}", "longitude": f"{lon:.3f}",
        "current": "temperature_2m,cloud_cover,precipitation,weather_code",
        "timezone": "auto",
    })
    request = urllib.request.Request(
        "https://api.open-meteo.com/v1/forecast?" + query,
        headers={"User-Agent": "R1World/1.0"})
    with urllib.request.urlopen(request, timeout=5) as response:
        data = json.load(response)
    current = data["current"]
    offset = int(data["utc_offset_seconds"])
    cloud = float(current["cloud_cover"])
    if not (-43200 <= offset <= 50400 and 0 <= cloud <= 100):
        raise ValueError("Invalid local conditions")
    return {
        "lon": lon, "lat": lat, "fetchedAt": time.time(),
        "timezone": str(data["timezone"]), "utcOffsetSeconds": offset,
        "weather": {
            "temperature": float(current["temperature_2m"]),
            "cloudCover": cloud,
            "precipitation": float(current["precipitation"]),
            "code": int(current["weather_code"]),
        },
    }


def _display(saved: dict | None, lon: float, lat: float) -> dict:
    age = time.time() - saved.get("fetchedAt", 0) if saved else math.inf
    fresh = saved is not None and 0 <= age < CURRENT_SECONDS
    known_zone = saved is not None and 0 <= age < 24 * 60 * 60
    return {
        "lon": lon, "lat": lat,
        "timezone": saved["timezone"] if known_zone else "",
        "utcOffsetSeconds": (saved["utcOffsetSeconds"] if known_zone else
                             round(lon / 15) * 3600),
        "timeSource": "zone" if known_zone else "longitude-approximation",
        "weather": saved["weather"] if fresh else None,
        "weatherSource": "forecast" if fresh else "unavailable",
    }


class LocalConditions:
    def __init__(self, session: Path, stopped: threading.Event, publish):
        self.session = session
        self.stopped = stopped
        self.publish = publish
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.thread.join(timeout=6)

    def _run(self):
        current_cell = None
        visible_weather = None
        visible_zone = None
        next_attempt = 0.0
        while not self.stopped.is_set():
            try:
                request = json.loads((self.session / "sea_request.json").read_text(encoding="utf-8"))
                lon, lat = float(request["lon"]), float(request["lat"])
                if not (math.isfinite(lon) and math.isfinite(lat)
                        and -180 <= lon <= 180 and -90 <= lat <= 90):
                    raise ValueError("Invalid conditions position")
                cell = _cell(lon, lat)
                if cell != current_cell:
                    current_cell = cell
                    visible_weather = None
                    visible_zone = None
                    next_attempt = 0.0
                cache = _cache_path(cell)
                try:
                    saved = json.loads(cache.read_text(encoding="utf-8"))
                except (OSError, ValueError, KeyError):
                    saved = None
                display = _display(saved, lon, lat)
                has_weather = display["weather"] is not None
                has_zone = display["timeSource"] == "zone"
                if visible_weather != has_weather or visible_zone != has_zone:
                    self.publish(self.session / "atmosphere.json", display)
                    visible_weather, visible_zone = has_weather, has_zone
                age = time.time() - saved.get("fetchedAt", 0) if saved else math.inf
                if age >= REFRESH_SECONDS and time.monotonic() >= next_attempt:
                    next_attempt = time.monotonic() + 60
                    try:
                        fresh = _forecast(*cell)
                        self.publish(cache, fresh)
                        self.publish(self.session / "atmosphere.json", _display(fresh, lon, lat))
                        visible_weather = visible_zone = True
                        print("WEATHER", cell, fresh["timezone"], flush=True)
                    except (OSError, ValueError, KeyError, TypeError) as error:
                        print("WEATHER-OFFLINE", cell, str(error), flush=True)
            except (OSError, ValueError, KeyError, TypeError):
                pass  # No position yet, or a request being replaced atomically.
            self.stopped.wait(2)
