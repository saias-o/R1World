"""Prefer a surveyed bare-earth DTM where available; persist every answer."""
import json
import math
from dataclasses import asdict
from .sources import ElevationGrid, fetch_elevation_grid, _request_json, _write_json

IGN_URL = "https://data.geopf.fr/altimetrie/1.0/calcul/alti/rest/elevation.json"
IGN_RESOURCE = "ign_rge_alti_wld"


def fetch_ground(bounds, folder, size=41):
    cache = folder/"ground-elevation.json"
    if cache.exists():
        doc = json.loads(cache.read_text(encoding="utf-8"))
        if doc["bounds"] != asdict(bounds):
            raise ValueError("Ground elevation cache bounds mismatch")
        return ElevationGrid(bounds,doc["size"],tuple(tuple(r) for r in doc["values"])),doc["source"]
    # A coarse eligibility box, not a claim of coverage: every IGN sample is
    # validated, and a no-data answer falls back for the whole tile.
    eligible = (-5.5 <= bounds.west and bounds.east <= 9.8 and
                41.2 <= bounds.south and bounds.north <= 51.2)
    if eligible:
        points = [(bounds.west+(bounds.east-bounds.west)*c/(size-1),
                   bounds.south+(bounds.north-bounds.south)*r/(size-1))
                  for r in range(size) for c in range(size)]
        payload = {"lon":"|".join(f"{p[0]:.10f}" for p in points),
                   "lat":"|".join(f"{p[1]:.10f}" for p in points),
                   "resource":IGN_RESOURCE,"delimiter":"|","zonly":"true"}
        try:
            doc = _request_json(IGN_URL,json.dumps(payload).encode(),timeout=20,attempts=2,
                                headers={"Content-Type":"application/json"})
            values = doc.get("elevations",[])
            if len(values)!=size*size or any(not isinstance(h,(int,float)) or
                    not math.isfinite(h) or h<=-1000 or h>9000 for h in values):
                raise ValueError("IGN returned incomplete or no-data terrain")
            rows = tuple(tuple(values[r*size:(r+1)*size]) for r in range(size))
            source = "IGN RGE ALTI bare-earth terrain via Geoplateforme"
            _write_json(cache,{"bounds":asdict(bounds),"size":size,"values":rows,"source":source})
            return ElevationGrid(bounds,size,rows),source
        except (OSError,ValueError,TypeError) as error:
            print("ELEVATION-FALLBACK",str(error),flush=True)
    legacy = folder/"elevation.json"
    refresh = False
    if legacy.exists():
        doc = json.loads(legacy.read_text(encoding="utf-8"))
        refresh = doc["bounds"] != asdict(bounds)
    grid = fetch_elevation_grid(bounds,7,legacy,refresh=refresh)
    source = "Copernicus DEM GLO-90 via Open-Meteo (fallback)"
    _write_json(cache,{"bounds":asdict(bounds),"size":grid.size,"values":grid.values,"source":source})
    return grid,source
