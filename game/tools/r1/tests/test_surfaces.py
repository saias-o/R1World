"""Surfaces: every swatch has a material, every material averages to its albedo.

`r1/surfaces.py` promises four things this file holds it to: that each ground
class, in each climate, and each wall and roof swatch of every region has a
photographed material; that the material's mean is the palette's measured
albedo (rule 2); that it is shown at its scan's real size; and that the cold
turns ground to frost and snow where it should.
"""

from __future__ import annotations

import math
import unittest

from r1 import atlas, ground, surfaces
from r1.mesh import Mesh, _tangents


def _profiles():
    return [v for v in vars(atlas).values() if isinstance(v, atlas.RegionProfile)]


class FamilyCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.families = surfaces.load_table()["families"]

    def test_every_family_is_built(self) -> None:
        self.assertEqual(set(self.families), set(surfaces.KINDS))
        for name, entry in self.families.items():
            for key in ("albedo", "normal", "mr"):
                self.assertTrue((surfaces.GAME_ROOT / entry[key]).is_file(), f"{name} {key}")
            self.assertLessEqual(entry["clipped"], 0.02, name)

    def test_every_ground_class_in_every_climate_has_a_material(self) -> None:
        for name, _swatch, _tags in ground.GROUND_CLASSES:
            for climate in surfaces.CLIMATES:
                family = surfaces.ground_family(name, "Ground, temperate", climate)
                if name == "water":
                    self.assertIsNone(family)
                else:
                    self.assertIn(family, self.families, f"{name} in {climate}")

    def test_every_region_ground_has_a_material(self) -> None:
        for profile in _profiles():
            family = surfaces.ground_family(ground.INFERRED, profile.ground.name, profile.climate)
            self.assertIn(family, self.families, profile.name)
            self.assertIn(profile.climate, surfaces.CLIMATES, profile.name)

    def test_every_wall_and_roof_swatch_has_a_material(self) -> None:
        for profile in _profiles():
            for swatch in profile.walls:
                self.assertEqual(self.families[surfaces.wall_family(swatch.name)]["kind"], "wall",
                                 swatch.name)
            for swatch in profile.roofs:
                self.assertEqual(self.families[surfaces.roof_family(swatch.name)]["kind"], "roof",
                                 swatch.name)

    def test_materials_are_what_their_names_say(self) -> None:
        self.assertEqual(surfaces.wall_family("Red brick"), "brick_red")
        self.assertEqual(surfaces.wall_family("London stock brick"), "brick_buff")
        self.assertEqual(surfaces.wall_family("Pierre de taille, cream"), "stone")
        self.assertEqual(surfaces.wall_family("Falu red timber"), "siding")
        self.assertEqual(surfaces.wall_family("Weathered larch"), "timber")
        self.assertEqual(surfaces.roof_family("Ardoise, blue-black"), "slate")
        self.assertEqual(surfaces.roof_family("Zinc, grey"), "metal_seam")
        self.assertEqual(surfaces.roof_family("Coppi, terracotta"), "tile_canal")
        self.assertEqual(surfaces.roof_family("Corrugated steel, rusted"), "corrugated")
        self.assertEqual(surfaces.roof_family("Roof terrace, whitewashed"), "terrace")

    def test_the_climates_ask_for_their_own_ground(self) -> None:
        self.assertEqual(surfaces.ground_family("grass", "", "temperate"), "grass")
        self.assertEqual(surfaces.ground_family("grass", "", "mediterranean"), "grass_dry")
        self.assertEqual(surfaces.ground_family("grass", "", "tropical"), "grass_lush")
        self.assertEqual(surfaces.ground_family("forest", "", "temperate"), "forest_temperate")
        self.assertEqual(surfaces.ground_family("forest", "", "boreal"), "forest_boreal")
        self.assertEqual(surfaces.ground_family("forest", "", "tropical"), "forest_tropical")
        self.assertEqual(surfaces.ground_family("sand", "", "arid"), "sand_desert")
        self.assertEqual(surfaces.ground_family("inferred", "Ground, desert sand", "arid"),
                         "sand_desert")
        self.assertEqual(surfaces.ground_family("grass@frost", "", "temperate"), "frost")
        self.assertEqual(surfaces.ground_family("forest@snow", "", "temperate"), "snow")
        self.assertIsNone(surfaces.ground_family("water@snow", "", "polar"))


class ColdTests(unittest.TestCase):
    def test_snowline_falls_with_latitude(self) -> None:
        self.assertEqual(surfaces.snowline(0.0), 5500.0)
        self.assertAlmostEqual(surfaces.snowline(45.9), 2760.0, delta=10.0)   # the Alps
        self.assertAlmostEqual(surfaces.snowline(-60.0), 1269.0, delta=10.0)
        self.assertEqual(surfaces.snowline(80.0), 0.0)

    def test_snow_above_frost_below(self) -> None:
        self.assertEqual(surfaces.cold_suffix(45.9, 3800.0, "temperate"), "@snow")
        self.assertEqual(surfaces.cold_suffix(45.9, 2600.0, "temperate"), "@frost")
        self.assertEqual(surfaces.cold_suffix(45.9, 1035.0, "temperate"), "")
        self.assertEqual(surfaces.cold_suffix(69.6, 10.0, "polar"), "@frost")

    def test_latitude_makes_climates_colder(self) -> None:
        self.assertEqual(surfaces.climate_at("temperate", 48.9), "temperate")
        self.assertEqual(surfaces.climate_at("temperate", 61.0), "boreal")
        self.assertEqual(surfaces.climate_at("boreal", 69.6), "polar")
        self.assertEqual(surfaces.climate_at("arid", 31.0), "arid")

    def test_cold_swatches_are_albedos(self) -> None:
        swatch = ground.material_for("grass@snow", atlas.PARIS)
        self.assertGreaterEqual(min(swatch.color), 0.80)
        self.assertEqual(ground.material_for("water@snow", atlas.PARIS).name, "Water")
        self.assertEqual(ground.material_for("urban@frost", atlas.PARIS).name, "Made ground")


class MaterialTests(unittest.TestCase):
    def test_the_rendered_mean_is_the_palette_albedo(self) -> None:
        table = surfaces.load_table()
        for family, entry in table["families"].items():
            colour = (0.21, 0.19, 0.15)
            material = surfaces.material("x", colour, 0.9, family, table=table)
            for c, factor in zip(colour, material.color[:3]):
                self.assertAlmostEqual(factor * entry["level"], c, places=9, msg=family)
            self.assertAlmostEqual(material.uv_scale, 1.0 / entry["uvSize"], places=12)
            self.assertTrue(material.normal_texture.startswith(surfaces.URI_PREFIX))

    def test_no_family_is_a_flat_colour(self) -> None:
        material = surfaces.material("Water", (0.02, 0.03, 0.05), 0.08, None)
        self.assertIsNone(material.base_color_texture)
        self.assertEqual(material.color[:3], (0.02, 0.03, 0.05))


class UvTests(unittest.TestCase):
    def test_a_roof_face_welds_like_it_did_without_uvs(self) -> None:
        # A pitched quad split in two triangles whose normals differ only by
        # roundoff: four vertices, as before UVs existed.
        mesh = Mesh(uv_mode="slope")
        a, b, c, d = (0.0, 0.0, 0.0), (7.3, 0.0, 0.1), (7.3, 3.1, 4.7), (0.0, 3.1, 4.6)
        mesh.add_up_quad(a, b, c, d)
        self.assertEqual(len(mesh.positions), 4)

    def test_slope_uvs_run_along_the_eave_and_up_the_slope(self) -> None:
        mesh = Mesh(uv_mode="slope")
        mesh.add_up_triangle((0.0, 0.0, 0.0), (4.0, 0.0, 0.0), (0.0, 3.0, 4.0))
        uv = {p: t for p, t in zip(mesh.positions, mesh.texcoords)}
        eave = abs(uv[(4.0, 0.0, 0.0)][0] - uv[(0.0, 0.0, 0.0)][0])
        slope = abs(uv[(0.0, 3.0, 4.0)][1] - uv[(0.0, 0.0, 0.0)][1])
        self.assertAlmostEqual(eave, 4.0, places=4)
        self.assertAlmostEqual(slope, 5.0, places=4)   # metres along the pitch

    def test_planar_uvs_are_metres_on_the_ground(self) -> None:
        mesh = Mesh(uv_mode="planar")
        mesh.add_up_triangle((1.0, 0.0, 2.0), (3.0, 0.5, 2.0), (1.0, 0.2, -1.0))
        self.assertEqual(mesh.texcoords[0], (1.0, -2.0))

    def test_tangents_are_unit_and_in_the_surface(self) -> None:
        mesh = Mesh(uv_mode="slope")
        mesh.add_up_quad((0.0, 0.0, 0.0), (5.0, 0.0, 0.0), (5.0, 2.0, 3.0), (0.0, 2.0, 3.0))
        for (tx, ty, tz, w), n in zip(_tangents(mesh.positions, mesh.normals, mesh.texcoords,
                                                mesh.indices), mesh.normals):
            self.assertAlmostEqual(math.sqrt(tx * tx + ty * ty + tz * tz), 1.0, places=6)
            self.assertAlmostEqual(tx * n[0] + ty * n[1] + tz * n[2], 0.0, places=6)
            self.assertIn(w, (-1.0, 1.0))


if __name__ == "__main__":
    unittest.main()
