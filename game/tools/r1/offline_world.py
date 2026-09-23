"""Coarse land and sea for a playable world without remote observations.

Natural Earth is bundled for the selection map. Its 1:110m outline says only
which side of the coast a tile lies on; it cannot supply local roads, buildings
or elevation. Offline tiles keep that limitation explicit in their manifest.
"""

import json
from functools import lru_cache
from pathlib import Path

from shapely.geometry import box, shape
from shapely.ops import unary_union

LAND = Path(__file__).resolve().parents[2] / "assets" / "world" / "land.geojson"


@lru_cache(maxsize=1)
def _land():
    document = json.loads(LAND.read_text(encoding="utf-8"))
    return unary_union(shape(feature["geometry"]) for feature in document["features"])


def sea_in(bounds):
    tile = box(bounds.west, bounds.south, bounds.east, bounds.north)
    sea = tile.difference(_land())
    return None if sea.is_empty else sea
