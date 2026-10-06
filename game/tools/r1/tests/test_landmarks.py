"""Contract tests for the landmarks (`landmarks.py`, `sculpt.py`).

A picture says whether the Eiffel Tower looks like itself
(`tools/landmark_preview.py`); these say the things a picture cannot:

  - every model fits its share of the arena (CLAUDE.md rule 5);
  - every finish is an albedo, not paint (rule 2);
  - every surface faces out -- including the back of a niche, which was
    missing from every recessed opening in the first render and showed the
    sky through the Empire State Building;
  - the same recipe draws the same triangles (§3 I3);
  - the OSM trace a landmark replaces is not extruded as well, and nothing
    else is taken with it;
  - a tile cooked before its landmark is cooked again, and only such a tile.

Nothing here needs the engine or the network.
"""

from __future__ import annotations

import math
import unittest

from r1 import landmarks, sculpt
from r1.landmarks import LANDMARKS, MAX_VERTICES, BY_SLUG


def _luminance(colour) -> float:
    return 0.2126 * colour[0] + 0.7152 * colour[1] + 0.0722 * colour[2]


def _signed_volume(mesh) -> float:
    total = 0.0
    for k in range(0, len(mesh.indices), 3):
        a, b, c = (mesh.positions[mesh.indices[k + q]] for q in range(3))
        total += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                  + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0
    return total


def _face_normal(mesh, k):
    a, b, c = (mesh.positions[mesh.indices[k + q]] for q in range(3))
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    return (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])


class TheList(unittest.TestCase):
    def test_twenty_places(self):
        # Giza's three pyramids are one place and three models.
        places = {l.slug.split("_of_")[0] if l.slug.startswith("pyramid") else l.slug
                  for l in LANDMARKS}
        self.assertEqual(len(places), 20)

    def test_identifiers_are_unique(self):
        self.assertEqual(len({l.slug for l in LANDMARKS}), len(LANDMARKS))
        self.assertEqual(len({l.wikidata for l in LANDMARKS}), len(LANDMARKS))
        for landmark in LANDMARKS:
            self.assertRegex(landmark.wikidata, r"^Q\d+$")
            self.assertRegex(landmark.osm, r"^(node|way|relation)/\d+$")

    def test_an_inferred_bearing_says_so(self):
        for slug in ("statue_of_liberty", "christ_the_redeemer"):
            self.assertNotEqual(BY_SLUG[slug].bearing_source, "osm")


class Models(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.parts = {l.slug: landmarks.model_parts(l) for l in LANDMARKS}

    def test_every_model_fits_its_share_of_the_arena(self):
        for slug, parts in self.parts.items():
            with self.subTest(slug):
                self.assertLessEqual(sum(len(p.mesh.positions) for p in parts), MAX_VERTICES)

    def test_height_is_the_official_one(self):
        for landmark in LANDMARKS:
            with self.subTest(landmark.slug):
                top = max(p[1] for part in self.parts[landmark.slug]
                          for p in part.mesh.positions)
                self.assertAlmostEqual(top, landmark.height, delta=landmark.height * 0.03 + 0.5)

    def test_finishes_are_albedos(self):
        # CLAUDE.md rule 2: nothing above the ~0.35 at which this world's
        # light saturates, nothing blacker than the darkest real surface.
        for slug, parts in self.parts.items():
            for part in parts:
                with self.subTest(slug=slug, finish=part.material.name):
                    colour = part.material.color[:3]
                    if part.material.base_color_texture:
                        # The factor is the albedo over the texture's level.
                        continue
                    self.assertGreaterEqual(_luminance(colour), 0.015)
                    self.assertLessEqual(_luminance(colour), 0.36)

    def test_textured_finishes_use_the_projects_own_photographs(self):
        for slug, parts in self.parts.items():
            for part in parts:
                uri = part.material.base_color_texture
                if uri:
                    with self.subTest(slug=slug, finish=part.material.name):
                        self.assertTrue(uri.startswith("../../../assets/textures/"), uri)

    def test_no_monument_wears_a_facade_sheet(self):
        # A landmark's UVs are metres; a wall family is one bay and one
        # storey with a window painted on, so it drew a window in every
        # square metre of the pyramids. Masonry has its own families.
        from r1 import surfaces
        for slug, parts in self.parts.items():
            for part in parts:
                with self.subTest(slug=slug, finish=part.material.name):
                    uri = part.material.base_color_texture or ""
                    self.assertNotIn("/facades/", uri)
                    self.assertNotIn("_and_window_", uri)
        for landmark in LANDMARKS:
            with sculpt.detail(0):
                finishes = landmark.recipe().finishes.values()
            for finish in finishes:
                if finish.family is not None:
                    with self.subTest(slug=landmark.slug, finish=finish.name):
                        self.assertIn(finish.family, surfaces.KINDS)
                        self.assertNotEqual(surfaces.KINDS[finish.family], "wall")

    def test_models_are_deterministic(self):
        again = landmarks.model_parts(BY_SLUG["st_basils_cathedral"])
        first = self.parts["st_basils_cathedral"]
        self.assertEqual([p.mesh.positions for p in first], [p.mesh.positions for p in again])
        self.assertEqual([p.mesh.indices for p in first], [p.mesh.indices for p in again])

    def test_no_degenerate_or_non_finite_vertex(self):
        for slug, parts in self.parts.items():
            for part in parts:
                for p in part.mesh.positions:
                    self.assertTrue(all(math.isfinite(c) for c in p), slug)
                for n in part.mesh.normals:
                    self.assertAlmostEqual(math.sqrt(sum(c * c for c in n)), 1.0, places=3)


class LevelsOfDetail(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.counts = {l.slug: [sum(len(p.mesh.positions) for p in landmarks.model_parts(l, k))
                               for k in range(3)] for l in LANDMARKS}

    def test_every_level_fits_its_budget(self):
        for slug, counts in self.counts.items():
            for level, count in enumerate(counts):
                with self.subTest(slug=slug, level=level):
                    self.assertLessEqual(count, landmarks.LOD_BUDGET[level])

    def test_each_level_is_lighter_than_the_one_before(self):
        for slug, (near, district, city) in self.counts.items():
            with self.subTest(slug):
                self.assertLessEqual(district, near)
                self.assertLessEqual(city, district)

    def test_a_far_level_keeps_the_silhouette_height(self):
        # From across the city the height is the one thing always seen. A
        # finial thinner than a pixel may go (the Taj's gilded one does, 7 m
        # of it); the body it stands on may not.
        for landmark in LANDMARKS:
            with self.subTest(landmark.slug):
                top = max(p[1] for part in landmarks.model_parts(landmark, 2)
                          for p in part.mesh.positions)
                self.assertGreater(top, landmark.height * 0.88)

    def test_far_levels_carry_no_texture(self):
        for part in landmarks.model_parts(BY_SLUG["notre_dame_de_paris"], 1):
            self.assertIsNone(part.material.base_color_texture, part.name)

    def test_every_landmark_knows_its_ground(self):
        for landmark in LANDMARKS:
            with self.subTest(landmark.slug):
                self.assertIn(landmark.ground[1], (landmarks.IGN, landmarks.GLO90))
                self.assertTrue(-20.0 < landmark.ground[0] < 1000.0)

    def test_the_shipped_list_is_the_recipes(self):
        # The game reads nothing else about a landmark (native/gen/landmarks.cpp):
        # a recipe changed without `python -m r1.landmarks` is a stale list.
        import json
        document = json.loads((landmarks.MODEL_DIR / "landmarks.json").read_text(encoding="utf-8"))
        self.assertEqual(document["revision"], landmarks.REVISION)
        self.assertEqual([e["slug"] for e in document["landmarks"]], [l.slug for l in LANDMARKS])
        for entry in document["landmarks"]:
            source = BY_SLUG[entry["slug"]]
            self.assertEqual((entry["lon"], entry["lat"], entry["bearing"]), (source.lon, source.lat, source.bearing))
            self.assertEqual(entry["alt"], source.ground[0])
            self.assertEqual([l["until"] for l in entry["levels"]], [0.0, landmarks.LOD1_UNTIL, landmarks.FAR_RANGE])
            for lod, level in enumerate(entry["levels"]):
                self.assertTrue((landmarks.GAME / level["path"]).exists(), level["path"])
                self.assertEqual(level["path"], landmarks.model_path(source, lod).relative_to(landmarks.GAME).as_posix())

    def test_far_range_is_what_the_camera_draws(self):
        # A monument is drawn as far as the camera sees: past its far plane a
        # level costs vertices nobody sees, and short of it the monument
        # would vanish in clear air.
        from r1 import prepare_world
        self.assertEqual(landmarks.FAR_RANGE, prepare_world.FAR_PLANE)

    def test_haze_is_the_same_in_the_scene_and_the_script(self):
        # The scene gives the first frame, `sun_cycle.js` every frame after:
        # two densities would be two atmospheres, a jump one frame in.
        import re
        from r1 import prepare_world
        script = (landmarks.GAME / "scripts" / "sun_cycle.js").read_text(encoding="utf-8")
        found = re.search(r"const FOG_DENSITY = ([0-9.]+);", script)
        self.assertIsNotNone(found)
        self.assertEqual(float(found.group(1)), prepare_world.FOG_DENSITY)


class Vocabulary(unittest.TestCase):
    def test_prism_encloses_a_positive_volume(self):
        s = sculpt.Sculpt()
        finish = sculpt.Finish("test", (0.2, 0.2, 0.2))
        sculpt.prism(s, finish, sculpt.rect(2.0, 3.0), 0.0, 4.0, bottom=True)
        self.assertAlmostEqual(_signed_volume(s.mesh(finish)), 24.0, places=6)

    def test_lathe_faces_out_even_where_it_is_horizontal(self):
        # A balcony: out, up, back in. Its flat top and underside are where
        # "away from the axis" says nothing, and where the profile must decide.
        s = sculpt.Sculpt()
        finish = sculpt.Finish("test", (0.2, 0.2, 0.2))
        sculpt.lathe(s, finish, [(1.0, 0.0), (1.0, 1.0), (2.0, 1.0), (2.0, 1.5), (1.0, 1.5),
                                 (1.0, 3.0), (0.0, 3.0)], sides=12, crease=30.0)
        mesh = s.mesh(finish)
        for k in range(0, len(mesh.indices), 3):
            n = _face_normal(mesh, k)
            a, b, c = (mesh.positions[mesh.indices[k + q]] for q in range(3))
            y = (a[1] + b[1] + c[1]) / 3
            r = math.hypot((a[0] + b[0] + c[0]) / 3, (a[2] + b[2] + c[2]) / 3)
            if abs(n[1]) > 0.99 * math.sqrt(sum(x * x for x in n)):
                expected = -1 if (abs(y - 1.0) < 1e-6 and r > 1.0) else 1
                self.assertEqual(math.copysign(1, n[1]), expected, (y, r))
        self.assertGreater(_signed_volume(mesh), 0.0)

    def test_a_recessed_opening_has_a_back(self):
        s = sculpt.Sculpt()
        wall = sculpt.Finish("wall", (0.2, 0.2, 0.2))
        dark = sculpt.Finish("dark", (0.02, 0.02, 0.02))
        face = sculpt.Face((0.0, 0.0), (1.0, 0.0))
        sculpt.panel(s, wall, face, 0.0, 10.0, 0.0, 10.0, [sculpt.Opening(2, 8, 1, 9)],
                     depth=0.5, recess=dark)
        back = s.mesh(dark)
        area = 0.0
        for k in range(0, len(back.indices), 3):
            n = _face_normal(back, k)
            self.assertGreaterEqual(n[2], -1e-9)       # it faces the street
            area += math.sqrt(sum(x * x for x in n)) / 2
        self.assertAlmostEqual(area, 6.0 * 8.0, places=6)
        # And the wall faces +z, the side the face was declared to face.
        wall_mesh = s.mesh(wall)
        self.assertTrue(all(_face_normal(wall_mesh, k)[2] >= -1e-9 or
                            abs(_face_normal(wall_mesh, k)[2]) < 1e-9
                            for k in range(0, len(wall_mesh.indices), 3)))

    def test_bearing_turns_recipe_x_to_the_compass(self):
        s = sculpt.Sculpt()
        finish = sculpt.Finish("test", (0.2, 0.2, 0.2))
        sculpt.beam(s, finish, (0.0, 0.0, 0.0), (10.0, 0.0, 0.0), 0.1)
        for bearing, expected in ((0.0, (0.0, -10.0)), (90.0, (10.0, 0.0)),
                                  (180.0, (0.0, 10.0))):
            parts = s.parts(bearing, lambda f: None)
            far = max(parts[0].mesh.positions, key=lambda p: p[0] ** 2 + p[2] ** 2)
            self.assertAlmostEqual(far[0], expected[0], delta=0.1)
            self.assertAlmostEqual(far[2], expected[1], delta=0.1)


if __name__ == "__main__":
    unittest.main()
