"""A place you have already been is yours, and it must never be fetched twice.

The requirement is the player's, not the code's: quit the game, come back, go
somewhere already visited, and nothing may touch the network. That holds today
-- a smoke run at Paris with both proxies pointed at a closed port passes with
an empty worker log, while the same run at an unvisited Buenos Aires fails with
both mirrors refused, which is the control that makes the first result mean
something.

Neither of those is a test, though: they need a GPU, a window and four minutes.
This file holds the same guarantee where it actually lives, by making the
network fatal and then asking for a cached tile anyway. Any future edit that
re-validates a cached tile against a server, re-downloads its sources to check
them, or "refreshes" it on a schedule fails here.
"""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from r1 import sources, world_service
from r1.world_tiles import Tile, tile_at


class _NetworkIsFatal:
    """Any socket at all is the failure this file is about."""

    def __call__(self, *args, **kwargs):
        raise AssertionError(
            "a cached tile reached the network; the offline guarantee is broken"
        )


class WorldCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.tile = tile_at(2.3522, 48.8566)
        self.folder = self.root / self.tile.key
        self.folder.mkdir(parents=True)
        self.payload = {
            "key": self.tile.key, "row": self.tile.row, "col": self.tile.col,
            "lon": self.tile.center[0], "lat": self.tile.center[1],
            "bounds": {"south": 0.0, "west": 0.0, "north": 1.0, "east": 1.0},
            "elevations": [[0.0, 0.0], [0.0, 0.0]], "footprints": [],
            "vertices": 1234, "buildings": 7, "surface": "land",
            "source": "OpenStreetMap; Copernicus DEM GLO-90 / Open-Meteo",
            "inference": {"measured_heights": 3, "inferred_heights": 4,
                          "omitted_invalid": 0},
        }
        (self.folder / "ready.json").write_text(json.dumps(self.payload), encoding="utf-8")

    def _offline(self):
        return mock.patch.object(sources.urllib.request, "urlopen", _NetworkIsFatal())

    def test_a_cooked_tile_is_served_from_disk(self) -> None:
        with self._offline(), mock.patch.object(world_service, "CACHE", self.root):
            self.assertEqual(world_service.cook(self.tile), self.payload)

    def test_it_survives_a_restart(self) -> None:
        # The cache is keyed by tile and version alone -- no session id, no spawn
        # point, no clock. That is the whole reason quitting and coming back
        # costs nothing, so it is asserted rather than assumed.
        self.assertNotIn("session", self.tile.key)
        with self._offline(), mock.patch.object(world_service, "CACHE", self.root):
            first = world_service.cook(self.tile)
            second = world_service.cook(Tile(self.tile.row, self.tile.col))
        self.assertEqual(first, second)
        self.assertEqual(first["buildings"], 7, "the cached count must survive intact")

    def test_the_raw_observations_are_kept_not_only_the_geometry(self) -> None:
        """Re-cooking must not mean re-downloading.

        `ready.json` is written last, after the mesh and the scene, so a run
        killed mid-cook leaves no ready file and the tile is cooked again. That
        second cook has to be offline too, which it only is because the OSM and
        elevation responses were cached beside it.
        """
        cached = {"elements": [], "r1QueryVersion": sources.OSM_QUERY_VERSION}
        (self.folder / "osm.json").write_text(json.dumps(cached), encoding="utf-8")
        (self.folder / "elevation.json").write_text(json.dumps(
            {"bounds": {"south": 0.0, "west": 0.0, "north": 1.0, "east": 1.0},
             "size": 2, "values": [[0.0, 0.0], [0.0, 0.0]]}), encoding="utf-8")
        with self._offline():
            grid = sources.fetch_elevation_grid(
                sources.Bounds(0.0, 0.0, 1.0, 1.0), 2, self.folder / "elevation.json")
            document = sources.fetch_osm(
                sources.Bounds(0.0, 0.0, 1.0, 1.0), self.folder / "osm.json")
        self.assertEqual(grid.size, 2)
        self.assertEqual(document, cached)

    def test_a_cache_answering_an_older_question_is_the_one_exception(self) -> None:
        """And it is an exception with a floor: never the network's word for it.

        A cached response is only as good as the query that produced it, and the
        cache key is a bounding box, which does not carry the query. When the
        question widens — v1 asked for buildings and roads, v2 also asks for the
        landcover classes rank 9 is made of — a cached v1 answer genuinely does
        not contain what is now being asked for, and re-asking is the correct
        behaviour rather than a broken promise.

        What must not follow is a world that disappears when the network does.
        So the stale document is returned rather than raised, and the tile
        records which question it was built from.
        """
        stale = {"elements": [{"type": "node", "id": 1, "lon": 0.0, "lat": 0.0}],
                 "r1QueryVersion": sources.OSM_QUERY_VERSION - 1}
        path = self.folder / "osm.json"
        path.write_text(json.dumps(stale), encoding="utf-8")
        refused = mock.Mock(side_effect=OSError("no route to host"))
        with mock.patch.object(sources.urllib.request, "urlopen", refused),                 mock.patch.object(sources.time, "sleep", lambda _seconds: None):
            document = sources.fetch_osm(
                sources.Bounds(0.0, 0.0, 1.0, 1.0), path)
        self.assertTrue(refused.called, "an older question must be re-asked once")
        self.assertEqual(document, stale, "and the old answer kept when nobody replies")
        self.assertEqual(json.loads(path.read_text(encoding="utf-8")), stale,
                         "a failed refresh must not damage what was on disk")

    def test_only_an_explicit_refresh_may_go_back_to_the_source(self) -> None:
        # The escape hatch has to exist -- OSM changes -- but it must never be
        # the default, or every restart re-downloads the planet.
        (self.folder / "osm.json").write_text(json.dumps({"elements": []}), encoding="utf-8")
        with self._offline():
            with self.assertRaises(AssertionError):
                sources.fetch_osm(
                    sources.Bounds(0.0, 0.0, 1.0, 1.0),
                    self.folder / "osm.json", refresh=True)
