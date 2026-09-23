"""Contract tests for the building chain (plan §10).

The chain infers most of what it builds, so almost none of it can be checked
against a known answer — there is no true height for an untagged building. What
*can* be checked, and is what the plan actually asks for, is the structure:

  - a measured value always beats an inferred one, and the manifest says which
    happened (§4 I5);
  - openings are aligned — same floor levels on every wall of a building, whole
    bays across each wall — because §10 says alignment is what carries the
    resemblance, above triangle count;
  - the geometry is *closed and outward*: a wall normal that points inward is
    invisible in a screenshot taken from the right side and obvious from the
    wrong one, and no other test in this project would catch it;
  - the same input produces the same building on every run (§4 I3).

The footprints below are written as literal metres in the engine's horizontal
plane, so the tests never touch a projection, an elevation grid or the network.
"""

from __future__ import annotations

import math
import unittest

from r1 import buildings
from r1.atlas import CHAMONIX, GENERIC
from r1.mesh import Mesh
from r1.sources import OsmWay
from r1.polygons import polygon_area


def square(side: float, origin: tuple[float, float] = (0.0, 0.0)) -> list[tuple[float, float]]:
    x, z = origin
    return [(x, z), (x + side, z), (x + side, z + side), (x, z + side)]


def rotate(ring, degrees: float, about=(0.0, 0.0)):
    angle = math.radians(degrees)
    cos, sin = math.cos(angle), math.sin(angle)
    cx, cz = about
    return [
        (cx + (x - cx) * cos - (z - cz) * sin, cz + (x - cx) * sin + (z - cz) * cos)
        for x, z in ring
    ]


def interior_angles(ring) -> list[float]:
    out = []
    count = len(ring)
    for index in range(count):
        previous = ring[index - 1]
        current = ring[index]
        following = ring[(index + 1) % count]
        a = (previous[0] - current[0], previous[1] - current[1])
        b = (following[0] - current[0], following[1] - current[1])
        na = math.hypot(*a)
        nb = math.hypot(*b)
        if na < 1e-9 or nb < 1e-9:
            continue
        cosine = max(-1.0, min(1.0, (a[0] * b[0] + a[1] * b[1]) / (na * nb)))
        out.append(math.degrees(math.acos(cosine)))
    return out


def flat_ground(_lon: float, _lat: float) -> tuple[float, float, float]:
    """A ground function whose only job is to be flat and boring."""
    return 0.0, 0.0, 0.0


def way(ring, osm_id: int = 1, tags: dict | None = None) -> OsmWay:
    """An OSM way carrying a footprint already expressed in metres.

    The chain calls `ground_of(lon, lat)` for each point and reads x and z off
    the result, so a ground function that returns the coordinates it was handed
    lets a test state a footprint in the units it cares about.
    """
    closed = tuple(ring) + (ring[0],)
    return OsmWay(osm_id=osm_id, points=closed, tags=tags or {})


def identity_ground(x: float, z: float) -> tuple[float, float, float]:
    return x, 0.0, z


def build(ring, tags=None, osm_id=1, profile=CHAMONIX, detail_radius=1e6):
    return buildings.build_buildings(
        [way(ring, osm_id, tags)], identity_ground, profile,
        detail_center=(0.0, 0.0), detail_radius=detail_radius,
    )


def triangles(parts, name_contains: str = "") -> list[tuple]:
    """Every triangle of the matching parts, as three world points."""
    out = []
    for part in parts:
        if name_contains and name_contains not in part.name:
            continue
        mesh: Mesh = part.mesh
        for i in range(0, len(mesh.indices), 3):
            out.append(tuple(mesh.positions[mesh.indices[i + k]] for k in range(3)))
    return out


def normal(tri) -> tuple[float, float, float]:
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    if length < 1e-12:
        return 0.0, 0.0, 0.0
    return nx / length, ny / length, nz / length


class FootprintTests(unittest.TestCase):
    """The hygiene the raw data needs before anything is extruded."""

    def test_welds_duplicate_vertices(self) -> None:
        ring = [(0.0, 0.0), (0.0, 0.001), (10.0, 0.0), (10.0, 10.0), (0.0, 10.0)]
        cleaned = buildings.clean_footprint(ring)
        self.assertIsNotNone(cleaned)
        self.assertEqual(len(cleaned), 4)

    def test_drops_a_vertex_that_does_not_turn(self) -> None:
        # A midpoint on a straight wall: OSM carries these constantly, and each
        # one would otherwise split the wall into two independent bay grids.
        ring = [(0.0, 0.0), (5.0, 0.0), (10.0, 0.0), (10.0, 8.0), (0.0, 8.0)]
        cleaned = buildings.clean_footprint(ring)
        self.assertEqual(len(cleaned), 4)

    def test_keeps_a_shallow_but_real_corner(self) -> None:
        # 30 cm off a 10 m chord is a corner, not noise, and dropping it would
        # straighten a splayed facade that is genuinely splayed.
        ring = [(0.0, 0.0), (5.0, -0.3), (10.0, 0.0), (10.0, 8.0), (0.0, 8.0)]
        cleaned = buildings.clean_footprint(ring)
        self.assertEqual(len(cleaned), 5)

    def test_rejects_a_mapping_artefact(self) -> None:
        self.assertIsNone(buildings.clean_footprint(square(0.8)))

    def test_keeps_a_garden_shed(self) -> None:
        # The threshold has to sit under the smallest real building, not under
        # the smallest comfortable one.
        self.assertIsNotNone(buildings.clean_footprint(square(1.6)))

    def test_orients_every_ring_the_same_way(self) -> None:
        # Half of OSM's closed ways are clockwise. Every outward normal in the
        # module is derived from a positive signed area, so this is the
        # precondition the rest of the file rests on.
        for ring in (square(10.0), list(reversed(square(10.0)))):
            cleaned = buildings.clean_footprint(ring)
            self.assertGreater(polygon_area(cleaned), 0.0)


class RectificationTests(unittest.TestCase):
    """Right angles traced by hand from imagery are not right."""

    def test_squares_up_a_hand_traced_rectangle(self) -> None:
        ring = [(0.0, 0.0), (12.1, 0.35), (11.7, 8.2), (-0.3, 7.9)]
        cleaned = buildings.clean_footprint(ring)
        for angle in interior_angles(cleaned):
            self.assertLess(abs(angle - 90.0), 1.0,
                            f"rectification left a {angle:.2f} deg corner")

    def test_a_rotated_building_stays_rotated(self) -> None:
        # Rectification snaps to the footprint's *own* grid, not to the world
        # axes: a building at 31 degrees to north must come out at 31 degrees.
        ring = rotate([(0.0, 0.0), (12.0, 0.3), (11.8, 8.0), (-0.2, 7.8)], 31.0)
        cleaned = buildings.clean_footprint(ring)
        for angle in interior_angles(cleaned):
            self.assertLess(abs(angle - 90.0), 1.0)
        edge = (cleaned[1][0] - cleaned[0][0], cleaned[1][1] - cleaned[0][1])
        bearing = math.degrees(math.atan2(edge[1], edge[0])) % 90.0
        self.assertLess(abs(bearing - 31.0), 2.0)

    def test_a_real_diagonal_survives(self) -> None:
        # A cut corner is a design, not a tracing error. Snapping it to the grid
        # would invent a building the survey did not find.
        ring = [(0.0, 0.0), (10.0, 0.0), (10.0, 6.0), (6.0, 10.0), (0.0, 10.0)]
        cleaned = buildings.clean_footprint(ring)
        self.assertEqual(len(cleaned), 5)
        angles = sorted(interior_angles(cleaned))
        self.assertLess(abs(angles[0] - 90.0), 1.5)
        self.assertGreater(angles[-1], 120.0)


class OrientedBoxTests(unittest.TestCase):
    """The box a ridge and an eave line are derived from."""

    def test_is_exact_for_a_rectangle(self) -> None:
        ring = buildings.clean_footprint([(0.0, 0.0), (12.0, 0.0), (12.0, 6.0), (0.0, 6.0)])
        box = buildings.oriented_box(ring)
        self.assertAlmostEqual(box.half_u, 6.0, places=5)
        self.assertAlmostEqual(box.half_v, 3.0, places=5)
        self.assertAlmostEqual(box.area, 72.0, places=4)

    def test_long_axis_follows_the_building(self) -> None:
        ring = buildings.clean_footprint(rotate(
            [(0.0, 0.0), (20.0, 0.0), (20.0, 7.0), (0.0, 7.0)], 40.0))
        box = buildings.oriented_box(ring)
        bearing = math.degrees(math.atan2(box.uz, box.ux)) % 180.0
        self.assertLess(min(abs(bearing - 40.0), abs(bearing - 220.0)), 1.5)
        self.assertGreater(box.half_u, box.half_v)

    def test_always_contains_the_footprint(self) -> None:
        # The roof is built over the box, so a footprint poking out of it would
        # be a wall with no roof above it.
        ring = buildings.clean_footprint(
            [(0.0, 0.0), (14.0, 2.0), (16.0, 9.0), (5.0, 12.0), (-2.0, 6.0)])
        box = buildings.oriented_box(ring)
        for x, z in ring:
            along = (x - box.cx) * box.ux + (z - box.cz) * box.uz
            across = (x - box.cx) * box.vx + (z - box.cz) * box.vz
            self.assertLessEqual(abs(along), box.half_u + 1e-6)
            self.assertLessEqual(abs(across), box.half_v + 1e-6)


class OrientationTests(unittest.TestCase):
    """Normals. The one property no screenshot reliably reveals."""

    def test_every_wall_faces_out(self) -> None:
        # A convex footprint, so "outward" is exactly "away from the centre".
        parts, _, _ = build(square(12.0, (-6.0, -6.0)), tags={"building:levels": "2"})
        checked = 0
        for tri in triangles(parts, "Walls"):
            n = normal(tri)
            if abs(n[1]) > 0.5:
                continue  # a cap or a gable soffit, not a wall
            centre = tuple(sum(p[axis] for p in tri) / 3.0 for axis in range(3))
            outward = (centre[0], 0.0, centre[2])
            length = math.hypot(outward[0], outward[2])
            if length < 1e-6:
                continue
            dot = (n[0] * outward[0] + n[2] * outward[2]) / length
            self.assertGreater(dot, 0.0, "a wall faces into the building")
            checked += 1
        self.assertGreater(checked, 8)

    def test_a_roof_has_an_upper_and_a_lower_surface(self) -> None:
        # The slab is what gives an eave its fascia and its soffit; without the
        # underside a deep overhang is a hole when seen from the street.
        parts, _, _ = build(
            [(0.0, 0.0), (14.0, 0.0), (14.0, 8.0), (0.0, 8.0)],
            tags={"building:levels": "2", "roof:shape": "gabled"},
        )
        up = down = 0
        for tri in triangles(parts, "Roofs"):
            n = normal(tri)
            if n[1] > 0.2:
                up += 1
            elif n[1] < -0.2:
                down += 1
        self.assertGreater(up, 0)
        self.assertGreater(down, 0, "the roof has no underside")

    def test_the_ridge_is_a_single_straight_line(self) -> None:
        parts, _, _ = build(
            [(0.0, 0.0), (18.0, 0.0), (18.0, 8.0), (0.0, 8.0)],
            tags={"building:levels": "2", "roof:shape": "gabled"},
        )
        peak = max(p[1] for tri in triangles(parts, "Roofs") for p in tri)
        ridge = [p for tri in triangles(parts, "Roofs") for p in tri
                 if abs(p[1] - peak) < 1e-6]
        self.assertGreaterEqual(len(ridge), 2)
        # Every point at ridge height shares one across-axis coordinate: that is
        # what "straight" means for a ridge, and it is the thing the eye checks.
        across = {round(p[2], 4) for p in ridge}
        self.assertEqual(len(across), 1, f"the ridge wanders across {across}")


class GabaritTests(unittest.TestCase):
    """Measured beats inferred, and the difference is recorded."""

    def _gabarit(self, tags, osm_id=7, ring=None):
        ring = buildings.clean_footprint(ring or [(0.0, 0.0), (12.0, 0.0), (12.0, 8.0), (0.0, 8.0)])
        box = buildings.oriented_box(ring)
        rectangularity = abs(polygon_area(ring)) / box.area
        return buildings.plan_gabarit(tags, osm_id, CHAMONIX, box, rectangularity)

    def test_a_shop_makes_a_building_taller_not_its_flats_shorter(self) -> None:
        ring = buildings.clean_footprint([(0.0, 0.0), (20.0, 0.0), (20.0, 12.0), (0.0, 12.0)])
        box = buildings.oriented_box(ring)
        rect = abs(polygon_area(ring)) / box.area
        plain = buildings.plan_gabarit({"building:levels": "3"}, 5, CHAMONIX, box, rect, False)
        shop = buildings.plan_gabarit({"building:levels": "3"}, 5, CHAMONIX, box, rect, True)
        self.assertGreater(shop.wall_height, plain.wall_height)
        self.assertEqual(shop.storeys, plain.storeys)

    def test_a_tagged_height_is_used_and_marked_measured(self) -> None:
        gabarit = self._gabarit({"height": "14"})
        self.assertEqual(gabarit.height_source, "tag:height")
        self.assertLessEqual(gabarit.wall_height + gabarit.roof_height, 14.01)

    def test_levels_become_metres_through_the_region(self) -> None:
        gabarit = self._gabarit({"building:levels": "4"})
        self.assertEqual(gabarit.height_source, "tag:levels")
        self.assertEqual(gabarit.storeys, 4)
        self.assertAlmostEqual(gabarit.wall_height, 4 * CHAMONIX.storey_height, places=5)

    def test_an_untagged_building_is_marked_inferred(self) -> None:
        gabarit = self._gabarit({})
        self.assertEqual(gabarit.height_source, "atlas")
        self.assertEqual(gabarit.roof_source, "atlas")

    def test_feet_are_metres(self) -> None:
        gabarit = self._gabarit({"height": "30 ft"})
        self.assertLess(abs((gabarit.wall_height + gabarit.roof_height) - 9.144), 0.6)

    def test_a_tagged_roof_shape_wins(self) -> None:
        gabarit = self._gabarit({"roof:shape": "hipped"})
        self.assertEqual(gabarit.roof_shape, "hipped")
        self.assertEqual(gabarit.roof_source, "tag:shape")

    def test_an_unbuildable_shape_maps_to_its_nearest_relative(self) -> None:
        # Wrong in the detail, right in the silhouette — which is the rank that
        # decides recognition (§2.1).
        self.assertEqual(self._gabarit({"roof:shape": "mansard"}).roof_shape, "hipped")

    def test_a_non_rectangular_footprint_refuses_a_ridge(self) -> None:
        # An L-shaped plan with a ridge across the notch looks worse than the
        # same plan with a flat roof, and the manifest has to say it happened.
        el = [(0.0, 0.0), (16.0, 0.0), (16.0, 5.0), (6.0, 5.0), (6.0, 14.0), (0.0, 14.0)]
        gabarit = self._gabarit({"roof:shape": "gabled"}, ring=el)
        self.assertEqual(gabarit.roof_shape, "flat")
        self.assertEqual(gabarit.roof_source, "atlas:not-rectangular")

    def test_a_building_too_wide_for_one_ridge_refuses_a_ridge(self) -> None:
        # A 40 m span at the region's pitch is an 11 m roof: a barn drawn on top
        # of a hotel. Until a multi-ridge generator exists, the building gets the
        # flat roof it most likely has.
        wide = [(0.0, 0.0), (60.0, 0.0), (60.0, 40.0), (0.0, 40.0)]
        gabarit = self._gabarit({"roof:shape": "gabled", "building:levels": "3"}, ring=wide)
        self.assertEqual(gabarit.roof_shape, "flat")
        self.assertEqual(gabarit.roof_source, "atlas:span")

    def test_an_inferred_roof_never_out_rises_its_walls(self) -> None:
        for osm_id in range(60):
            gabarit = self._gabarit({}, osm_id=osm_id)
            self.assertLessEqual(gabarit.roof_height, gabarit.wall_height + 1e-6)

    def test_a_measured_roof_height_is_left_alone(self) -> None:
        # A church spire is allowed to be a church spire; the cap is there to
        # discipline a guess, not to overrule a survey.
        gabarit = self._gabarit({"roof:shape": "pyramidal", "roof:height": "18",
                                 "height": "30"})
        self.assertGreater(gabarit.roof_height, gabarit.wall_height)

    def test_the_region_supplies_a_plausible_storey_count(self) -> None:
        counts = {self._gabarit({}, osm_id=i).storeys for i in range(200)}
        self.assertTrue(counts <= {2, 3, 4}, counts)
        self.assertGreater(len(counts), 1, "every untagged building got the same height")


class AlignmentTests(unittest.TestCase):
    """§10: alignment is worth more than ten times the triangles."""

    def _openings(self, parts):
        return triangles(parts, "glazing")

    def test_windows_on_different_walls_share_floor_levels(self) -> None:
        parts, _, _ = build(square(16.0, (-8.0, -8.0)),
                            tags={"building:levels": "3"})
        panes = self._openings(parts)
        self.assertGreater(len(panes), 0, "no openings were built")
        sills = sorted({round(min(p[1] for p in tri), 3) for tri in panes})
        # Three floors, so three distinct sill heights across the whole
        # building — not three per wall.
        self.assertEqual(len(sills), 3, f"sills at {sills}")

    def test_bays_divide_each_wall_evenly(self) -> None:
        parts, _, _ = build([(0.0, 0.0), (13.0, 0.0), (13.0, 9.0), (0.0, 9.0)],
                            tags={"building:levels": "1"})
        panes = self._openings(parts)
        # Group pane centres by wall (the south wall runs along x at z = 0).
        # The z = 0 wall. Its panes sit a reveal's depth behind the plane, so
        # the filter is a neighbourhood and not an equality. A pane's centre is
        # read off its extent rather than as a vertex mean: a quad arrives as
        # two triangles whose centroids are not the centre of the quad.
        south = sorted(
            (min(p[0] for p in tri) + max(p[0] for p in tri)) * 0.5
            for tri in panes
            if all(abs(p[2]) < 0.5 for p in tri)
        )
        centres = sorted({round(c, 3) for c in south})
        self.assertGreaterEqual(len(centres), 3)
        gaps = [b - a for a, b in zip(centres, centres[1:])]
        self.assertLess(max(gaps) - min(gaps), 1e-3, f"uneven bays: {gaps}")

    def test_a_narrow_bay_gets_no_window_rather_than_a_squeezed_one(self) -> None:
        # One squeezed opening destroys a facade's rhythm more surely than a
        # blank bay does, so the generator refuses.
        parts, _, _ = build([(0.0, 0.0), (1.6, 0.0), (1.6, 1.6), (0.0, 1.6)],
                            tags={"building:levels": "1"})
        self.assertEqual(len(self._openings(parts)), 0)

    def test_an_opening_is_recessed_not_painted_on(self) -> None:
        parts, _, _ = build(square(16.0, (-8.0, -8.0)), tags={"building:levels": "2"})
        panes = self._openings(parts)
        south = [tri for tri in panes if all(abs(p[2] + 8.0) < 0.5 for p in tri)]
        self.assertGreater(len(south), 0)
        # The pane sits behind the wall plane by the profile's reveal depth.
        depth = min(abs(p[2] + 8.0) for tri in south for p in tri)
        self.assertGreater(depth, CHAMONIX.window_inset * 0.5)

    def test_a_commercial_ground_floor_is_taller(self) -> None:
        plain = build(square(20.0, (-10.0, -10.0)), tags={"building:levels": "3"})[0]
        shop = build(square(20.0, (-10.0, -10.0)),
                     tags={"building:levels": "3", "shop": "bakery"})[0]
        plain_sills = sorted({round(min(p[1] for p in t), 2) for t in triangles(plain, "glazing")})
        shop_sills = sorted({round(min(p[1] for p in t), 2) for t in triangles(shop, "glazing")})
        self.assertEqual(len(plain_sills), 3)
        self.assertEqual(len(shop_sills), 3)
        # The first floor above a shopfront starts higher than it would above a
        # dwelling's ground floor.
        self.assertGreater(shop_sills[1], plain_sills[1])


class PartyWallTests(unittest.TestCase):
    """A window onto a neighbour's masonry is the error nobody screenshots."""

    def _shared_ways(self):
        # Two 10 m squares sharing the x = 10 wall exactly.
        left = way(square(10.0, (0.0, 0.0)), osm_id=1, tags={"building:levels": "2"})
        right = way(square(10.0, (10.0, 0.0)), osm_id=2, tags={"building:levels": "2"})
        return [left, right]

    def test_a_shared_wall_carries_no_openings(self) -> None:
        parts, _, stats = buildings.build_buildings(
            self._shared_ways(), identity_ground, CHAMONIX,
            detail_center=(10.0, 5.0), detail_radius=1e6,
        )
        self.assertGreaterEqual(stats.party_walls, 2)
        for tri in triangles(parts, "glazing"):
            for point in tri:
                self.assertGreater(abs(point[0] - 10.0), 0.5,
                                   "an opening was built on the party wall")

    def test_a_street_gap_is_not_a_party_wall(self) -> None:
        # Two houses four metres apart are two houses, and both have windows.
        left = way(square(10.0, (0.0, 0.0)), osm_id=1, tags={"building:levels": "2"})
        right = way(square(10.0, (14.0, 0.0)), osm_id=2, tags={"building:levels": "2"})
        _, _, stats = buildings.build_buildings(
            [left, right], identity_ground, CHAMONIX,
            detail_center=(7.0, 5.0), detail_radius=1e6,
        )
        self.assertEqual(stats.party_walls, 0)

    def test_a_building_does_not_share_a_wall_with_itself(self) -> None:
        # A concave footprint brings two of its own walls close together; that
        # is a plan, not a neighbour.
        el = [(0.0, 0.0), (16.0, 0.0), (16.0, 5.0), (6.0, 5.2), (6.0, 14.0), (0.0, 14.0)]
        _, _, stats = build(el, tags={"building:levels": "2"})
        self.assertEqual(stats.party_walls, 0)


class DetailRadiusTests(unittest.TestCase):
    """§12.4: the detail follows the speed, and the massing never does."""

    def test_a_distant_building_keeps_its_massing_and_loses_its_windows(self) -> None:
        far = [(400.0, 0.0), (412.0, 0.0), (412.0, 8.0), (400.0, 8.0)]
        parts, _, stats = buildings.build_buildings(
            [way(far, 3, {"building:levels": "2", "roof:shape": "gabled"})],
            identity_ground, CHAMONIX,
            detail_center=(0.0, 0.0), detail_radius=100.0,
        )
        self.assertEqual(stats.detailed, 0)
        self.assertEqual(len(triangles(parts, "glazing")), 0)
        self.assertGreater(len(triangles(parts, "Walls")), 0)
        self.assertGreater(len(triangles(parts, "Roofs")), 0, "the silhouette was dropped")


class DeterminismTests(unittest.TestCase):
    """§4 I3: the same description regenerates identically, everywhere."""

    def test_two_runs_produce_identical_geometry(self) -> None:
        ring = [(0.0, 0.0), (13.0, 0.0), (13.0, 9.0), (0.0, 9.0)]
        first = build(ring, tags={}, osm_id=424242)[0]
        second = build(ring, tags={}, osm_id=424242)[0]
        self.assertEqual([p.name for p in first], [p.name for p in second])
        for a, b in zip(first, second):
            self.assertEqual(a.mesh.positions, b.mesh.positions)
            self.assertEqual(a.mesh.indices, b.mesh.indices)

    def test_neighbouring_ids_do_not_draw_the_same_house(self) -> None:
        ring = [(0.0, 0.0), (13.0, 0.0), (13.0, 9.0), (0.0, 9.0)]
        names = set()
        for osm_id in range(1000, 1030):
            parts = build(ring, tags={}, osm_id=osm_id)[0]
            names.add(tuple(sorted(part.name for part in parts)))
        self.assertGreater(len(names), 3, "consecutive ids produced one house")

    def test_the_wall_palette_does_not_decide_the_roof(self) -> None:
        # Correlated draws produce visible runs of identical houses; the salts
        # in `atlas.seeded` exist to prevent exactly that.
        pairs = set()
        for osm_id in range(500, 700):
            wall = CHAMONIX.wall_swatch(buildings.seeded(osm_id, buildings.SALT_WALL)).name
            roof = CHAMONIX.roof_swatch(buildings.seeded(osm_id, buildings.SALT_ROOF)).name
            pairs.add((wall, roof))
        self.assertGreater(len(pairs), 8, f"walls and roofs are correlated: {pairs}")


class RegionTests(unittest.TestCase):
    """The Atlas seam (§9), while it holds twelve regions and twenty-two bands.

    These are contract tests, not content tests. There is no right answer to
    "what colour is Rome", and pinning one here would only make the table
    harder to correct. What is pinned is the shape of the answer: the most
    specific statement wins, the tier is honest about which kind of statement
    it is, and nowhere on the ellipsoid raises.
    """

    def test_chamonix_resolves_to_its_region(self) -> None:
        from r1.atlas import profile_for
        self.assertIs(profile_for(6.869433, 45.923697), CHAMONIX)

    def test_a_named_region_beats_the_band_it_sits_in(self) -> None:
        from r1.atlas import ALPINE, FRANCE, PARIS, profile_for
        # Both pairs sit inside a band's rectangle as well as a region's; the
        # region must win, or the table's ordering has been broken by an edit.
        self.assertIs(profile_for(2.3522, 48.8566), PARIS)
        self.assertIs(profile_for(6.869433, 45.923697), CHAMONIX)
        # And a point in the same band but outside the region falls to it.
        self.assertIs(profile_for(-1.55, 47.22), FRANCE)      # Nantes
        self.assertIs(profile_for(9.18, 46.10), ALPINE)       # Valteline

    def test_a_band_is_declared_as_a_band(self) -> None:
        from r1.atlas import profile_for
        for lon, lat in [(-58.38, -34.60), (77.21, 28.61), (36.82, -1.29),
                         (116.40, 39.90), (-95.37, 29.76)]:
            self.assertEqual(profile_for(lon, lat).tier, "band")

    def test_outside_every_rectangle_admits_it(self) -> None:
        from r1.atlas import profile_for
        # Mid-Atlantic and the Antarctic plateau: no band has anything to say
        # about either, and saying so is the point of the third tier (§4 I5).
        self.assertIs(profile_for(-30.0, 0.0), GENERIC)
        self.assertIs(profile_for(0.0, -80.0), GENERIC)

    def test_every_point_on_the_ellipsoid_resolves(self) -> None:
        from r1.atlas import profile_for
        for lat in range(-89, 90, 7):
            for lon in range(-180, 181, 11):
                profile = profile_for(float(lon), float(lat))
                self.assertIn(profile.tier, ("region", "band", "none"))
                # A profile with an empty palette would crash the generator at
                # the first building drawn from it, half a planet from here.
                self.assertTrue(profile.walls, profile.name)
                self.assertTrue(profile.roofs, profile.name)
                self.assertGreater(sum(w for _, w in profile.storey_weights), 0.0)
                self.assertGreater(sum(w for _, w in profile.roof_shape_weights), 0.0)

    def test_every_roof_shape_a_profile_can_draw_is_one_the_generator_builds(self) -> None:
        from r1.atlas import _REGIONS, GENERIC
        buildable = {"flat", "gabled", "hipped", "pyramidal", "skillion"}
        for _, profile in _REGIONS + (((0, 0, 0, 0), GENERIC),):
            for shape, _weight in profile.roof_shape_weights:
                self.assertIn(shape, buildable, f"{profile.name}: {shape}")


if __name__ == "__main__":
    unittest.main()
