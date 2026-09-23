import unittest

from r1.tiling import (
    BY_NAME,
    COMFORT_PRESET,
    LAYERS,
    REFERENCE_PRESET,
    from_quadkey,
    loaded_set_size,
    quadkey,
    tile_bounds,
    tile_of,
    tile_span_m,
    tiles_in_radius,
)


class TilingTests(unittest.TestCase):
    def test_layer_contract_matches_the_plan(self):
        expected = {
            "globe": (5, 20_000_000, 20_000_000, 4000.0),
            "region": (9, 300_000, 300_000, 240.0),
            "landscape": (12, 40_000, 60_000, 60.0),
            "local": (14, 6_000, 12_000, 15.0),
            "street": (16, 1_000, 2_000, 5.0),
            "detail": (17, 200, 400, 5.0),
        }
        self.assertEqual(set(expected), set(BY_NAME))
        for name, values in expected.items():
            layer = BY_NAME[name]
            self.assertEqual(
                (layer.zoom, layer.reference_radius_m, layer.comfort_radius_m,
                 layer.terrain_post_m),
                values,
            )

    def test_chamonix_is_inside_its_tile_and_quadkey_round_trips(self):
        lon, lat = 6.869433, 45.923697
        for layer in LAYERS:
            x, y = tile_of(lon, lat, layer.zoom)
            west, south, east, north = tile_bounds(x, y, layer.zoom)
            self.assertLessEqual(west, lon)
            self.assertLess(lon, east)
            self.assertLessEqual(south, lat)
            self.assertLess(lat, north)
            self.assertEqual(from_quadkey(quadkey(x, y, layer.zoom)), (x, y, layer.zoom))

    def test_reference_is_the_default_and_never_loads_more_than_comfort(self):
        explicit = loaded_set_size(45.923697, REFERENCE_PRESET)
        self.assertEqual(loaded_set_size(45.923697), explicit)
        comfort = loaded_set_size(45.923697, COMFORT_PRESET)
        for layer in LAYERS:
            self.assertLessEqual(explicit[layer.name], comfort[layer.name])

    def test_radius_results_are_unique_across_antimeridian(self):
        tiles = list(tiles_in_radius(179.999, 0.0, BY_NAME["detail"]))
        self.assertEqual(len(tiles), len(set(tiles)))
        self.assertTrue(all(0 <= x < 1 << z for x, _, z in tiles))

    def test_tile_width_shrinks_with_latitude(self):
        equator = tile_of(0.0, 0.0, 14)
        oslo = tile_of(10.75, 59.91, 14)
        self.assertGreater(
            tile_span_m(*equator, 14)[0],
            tile_span_m(*oslo, 14)[0],
        )

    def test_invalid_identifiers_are_rejected(self):
        with self.assertRaises(ValueError):
            from_quadkey("1204")
        with self.assertRaises(ValueError):
            tile_bounds(-1, 0, 14)
        with self.assertRaises(ValueError):
            tile_of(float("nan"), 0.0, 14)
        with self.assertRaises(ValueError):
            BY_NAME["street"].radius_m("unknown")


if __name__ == "__main__":
    unittest.main()
