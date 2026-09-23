import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from r1 import world_elevation as elevation
from r1.sources import Bounds, ElevationGrid


class GroundElevationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder=Path(self.temp.name)
        self.bounds=Bounds(48.85,2.35,48.86,2.36)

    def test_ign_answer_is_reused_without_network(self):
        with patch.object(elevation,"_request_json",return_value={"elevations":[34.,35.,36.,37.]}) as request:
            grid,source=elevation.fetch_ground(self.bounds,self.folder,size=2)
            self.assertEqual(grid.values,((34.,35.),(36.,37.)))
            self.assertIn("IGN",source)
            self.assertEqual(request.call_count,1)
        with patch.object(elevation,"_request_json",side_effect=AssertionError("network")):
            self.assertEqual(elevation.fetch_ground(self.bounds,self.folder)[0],grid)

    def test_no_data_falls_back_and_is_not_published_as_a_deep_hole(self):
        fallback=ElevationGrid(self.bounds,2,((3.,3.),(3.,3.)))
        with patch.object(elevation,"_request_json",return_value={"elevations":[-99999]*4}), \
             patch.object(elevation,"fetch_elevation_grid",return_value=fallback):
            grid,source=elevation.fetch_ground(self.bounds,self.folder,size=2)
        self.assertEqual(grid,fallback)
        self.assertIn("fallback",source)
        self.assertNotIn(-99999,json.loads((self.folder/"ground-elevation.json").read_text())["values"][0])

    def test_uncovered_region_does_not_query_ign(self):
        bounds=Bounds(35.,139.,35.01,139.01)
        fallback=ElevationGrid(bounds,2,((3.,3.),(3.,3.)))
        with patch.object(elevation,"_request_json",side_effect=AssertionError("IGN outside eligibility")), \
             patch.object(elevation,"fetch_elevation_grid",return_value=fallback):
            self.assertEqual(elevation.fetch_ground(bounds,self.folder)[0],fallback)
