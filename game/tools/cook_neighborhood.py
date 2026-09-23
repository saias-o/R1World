"""Rebuild a complete visual-QA neighbourhood from one bounded OSM survey."""
import argparse
import hashlib
import json
from dataclasses import asdict
from r1.sources import Bounds, fetch_osm
from r1.world_tiles import neighborhood
from r1.world_service import CACHE, cook


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("lon",type=float)
    parser.add_argument("lat",type=float)
    parser.add_argument("--rebuild",action="store_true")
    args = parser.parse_args()
    tiles = neighborhood(args.lon,args.lat)
    boxes = [t.bounds for t in tiles]
    bounds = Bounds(min(b.south for b in boxes),min(b.west for b in boxes),
                    max(b.north for b in boxes),max(b.east for b in boxes))
    source = None
    if bounds.east-bounds.west < .15:
        key = hashlib.sha256(json.dumps(asdict(bounds),sort_keys=True).encode()).hexdigest()[:20]
        source = CACHE/"sources"/(key+".json")
        fetch_osm(bounds,source)
    total = 0
    for tile in tiles:
        if args.rebuild:
            (CACHE/tile.key/"ready.json").unlink(missing_ok=True)
        result = cook(tile,source)
        total += result["vertices"]
        print(json.dumps({k:result[k] for k in ("key","vertices","streets")}),flush=True)
    print("Resident geometry:",total,"/ 900000",flush=True)
    if total > 900000:
        raise RuntimeError("Neighbourhood exceeds the runtime resident budget")


if __name__ == "__main__":
    main()
