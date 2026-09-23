"""`scripts/sun_cycle.js` must compute exactly what `r1/solar.py` computes.

The runtime sun cycle is a hand port of the Python solar module into QuickJS,
and the README, the plan and the script's own header all claim the two are the
same model rather than two tunings of one idea. That claim decays the moment
either file is edited alone, and nothing else in the project would notice: the
Python tests exercise the Python, and the engine never runs the Python.

So this file runs the JavaScript. It loads `sun_cycle.js`, supplies the four
globals the engine would (`exportProperty`, `props`, `node`, `tree`, `scene`),
and
compares its ephemerides, its air mass, its transmittance, its light colour and
intensity and its sky terms against `solar.py` over a year of instants at five
latitudes from the equator to inside the Arctic circle — the range where the
polar branches of the model differ from the temperate ones.

The tolerance is 1e-9 relative, which is a port-fidelity tolerance and not a
physics one: both sides are IEEE doubles evaluating the same expressions in the
same order, so anything larger means the expressions have drifted apart.

The script is loaded, never modified: the harness wraps it in a function and
asks for its bindings back, so no test scaffolding lives in the shipped file.
If Node is not installed the test skips — it is a contract test for a file the
engine runs, and a missing developer tool is not a failure of that contract.
"""

from __future__ import annotations

import json
import math
import shutil
import subprocess
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from r1 import skies, solar

SCRIPT = Path(__file__).resolve().parents[3] / "scripts" / "sun_cycle.js"

# An alpine valley at 1 035 m, so the altitude and turbidity terms are exercised
# rather than left at sea level.
ANCHOR_LON = 6.869433
ANCHOR_LAT = 45.923697
ALTITUDE = 1035.0
TURBIDITY = 2.1
PEAK_INTENSITY = solar.PEAK_INTENSITY

# Latitudes chosen for the branches they exercise, not for coverage of a map:
# the equator (no seasonal swing), the Alps, mid-latitude, the Arctic circle
# edge, and Tromso (midnight sun and polar night, where `hour_angle_at_elevation`
# has no solution and the smoothsteps sit at their ends for weeks).
LATITUDES = (0.0, ANCHOR_LAT, 51.5, 66.6, 69.65)

TOLERANCE = 1e-9

HARNESS = r"""
const fs = require("fs");
const source = fs.readFileSync(process.argv[2], "utf8");
const cases = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));

// The engine's globals, reduced to what the script touches at load time.
// `props` is filled by `exportProperty` exactly as ScriptBehaviour fills it
// from the scene, then overridden by the case's parameters.
function makeModule(params) {
    const props = {};
    const exportProperty = (name, value) => { props[name] = value; };
    const node = { setProperty: () => true, setRotation: () => {} };
    const tree = { firstInGroup: () => null };
    const scene = { getSetting: () => null, setSetting: () => true };
    // Two passes: the first collects the declared properties, the second runs
    // the file with the case's values in place, which is the order the engine
    // uses (declaration, then scene overrides, then module body).
    new Function("exportProperty", "props", "node", "tree", source + "\n")(
        exportProperty, props, node, tree);
    const bound = Object.assign({}, props, params);
    return new Function(
        "exportProperty", "props", "node", "tree",
        source + "\nreturn { sunAngles, sunLight, skyLight, skyState, airMass, transmittance };\n"
    )(() => {}, bound, node, tree);
}

const out = [];
for (const c of cases) {
    const m = makeModule(c.params);
    const angles = m.sunAngles(c.unix);
    const light = m.sunLight(c.unix);
    const sky = m.skyLight(angles.elevation);
    const dome = m.skyState(light, sky.horizonColor);
    out.push({
        azimuth: angles.azimuth,
        elevation: angles.elevation,
        trueElevation: angles.trueElevation,
        trueSolarTime: angles.trueSolarTime,
        airMass: m.airMass(angles.elevation, c.params.altitude),
        transmittance: m.transmittance(angles.elevation, c.params.altitude, c.params.turbidity),
        direction: light.direction,
        color: light.color,
        intensity: light.intensity,
        ambient: sky.ambient,
        horizonColor: sky.horizonColor,
        iblIntensity: sky.iblIntensity,
        dome: dome,
    });
}
process.stdout.write(JSON.stringify(out));
"""


def _instants() -> list[datetime]:
    """A year sampled every 11 days and 37 minutes.

    The offset is deliberate: a whole number of days would sample the same
    solar time every time and never cross a sunrise, and the point is to land
    inside the transitions — the visibility fade, the twilight smoothsteps and
    the refraction branches — not only in broad daylight.
    """
    start = datetime(2026, 1, 1, 0, 13, tzinfo=timezone.utc)
    return [start + timedelta(days=11 * i, minutes=37 * i) for i in range(34)]


def _node() -> str | None:
    return shutil.which("node")


class SunCycleParityTests(unittest.TestCase):
    """The JS port and the Python module agree to 1e-9 on every term."""

    @classmethod
    def setUpClass(cls) -> None:
        node = _node()
        if node is None:
            raise unittest.SkipTest("node is not installed; JS parity cannot be checked")
        cls.cases = [
            {
                "unix": when.timestamp(),
                "params": {
                    "anchorLon": ANCHOR_LON,
                    "anchorLat": lat,
                    "epochUnix": when.timestamp(),
                    "secondsPerSecond": 60.0,
                    "turbidity": TURBIDITY,
                    "altitude": ALTITUDE,
                    "peakIntensity": PEAK_INTENSITY,
                },
                "when": when,
                "lat": lat,
            }
            for lat in LATITUDES
            for when in _instants()
        ]
        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "parity.js"
            harness.write_text(HARNESS, encoding="utf8")
            payload = Path(tmp) / "cases.json"
            payload.write_text(
                json.dumps([{"unix": c["unix"], "params": c["params"]} for c in cls.cases]),
                encoding="utf8",
            )
            result = subprocess.run(
                [node, str(harness), str(SCRIPT), str(payload)],
                capture_output=True, text=True, check=False,
            )
        if result.returncode != 0:
            raise AssertionError(f"sun_cycle.js failed to run:\n{result.stderr}")
        cls.actual = json.loads(result.stdout)

    def _close(self, got: float, want: float, what: str, case: dict) -> None:
        scale = max(abs(want), 1.0)
        self.assertLessEqual(
            abs(got - want) / scale, TOLERANCE,
            f"{what} diverges at lat {case['lat']} on {case['when'].isoformat()}: "
            f"JS {got!r} vs Python {want!r}",
        )

    def test_ephemerides_match(self) -> None:
        for case, js in zip(self.cases, self.actual):
            azimuth, elevation, true_elevation, _, _ = solar.sun_angles(
                ANCHOR_LON, case["lat"], case["when"]
            )
            self._close(js["azimuth"], azimuth, "azimuth", case)
            self._close(js["elevation"], elevation, "elevation", case)
            self._close(js["trueElevation"], true_elevation, "true elevation", case)

    def test_true_solar_time_matches(self) -> None:
        # Not a public Python function, so it is checked through the identity
        # that defines it: the hour angle it produces must be the one the
        # elevation was computed from. A drift here is a sign error in the
        # longitude or equation-of-time term, which no elevation check catches
        # at the anchor's own longitude.
        for case, js in zip(self.cases, self.actual):
            hour_angle = js["trueSolarTime"] / 4.0 - 180.0
            phi, delta = math.radians(case["lat"]), math.radians(
                solar._solar_geometry(solar._julian_century(case["when"]))[0]
            )
            cos_zenith = (math.sin(phi) * math.sin(delta)
                          + math.cos(phi) * math.cos(delta) * math.cos(math.radians(hour_angle)))
            elevation = 90.0 - math.degrees(math.acos(max(-1.0, min(1.0, cos_zenith))))
            self._close(elevation, js["trueElevation"], "true solar time", case)

    def test_air_mass_and_transmittance_match(self) -> None:
        for case, js in zip(self.cases, self.actual):
            _, elevation, _, _, _ = solar.sun_angles(ANCHOR_LON, case["lat"], case["when"])
            self._close(js["airMass"], solar.air_mass(elevation, ALTITUDE), "air mass", case)
            want = solar.transmittance(elevation, ALTITUDE, TURBIDITY)
            for channel, (got, expected) in enumerate(zip(js["transmittance"], want)):
                self._close(got, expected, f"transmittance[{channel}]", case)

    def test_light_matches(self) -> None:
        for case, js in zip(self.cases, self.actual):
            state = solar.sun_state(
                ANCHOR_LON, case["lat"], case["when"], ALTITUDE, TURBIDITY, PEAK_INTENSITY
            )
            for axis, (got, expected) in enumerate(zip(js["direction"], state.direction)):
                self._close(got, expected, f"direction[{axis}]", case)
            for channel, (got, expected) in enumerate(zip(js["color"], state.color)):
                self._close(got, expected, f"color[{channel}]", case)
            self._close(js["intensity"], state.intensity, "intensity", case)

    def test_sky_terms_match(self) -> None:
        # These reach the renderer through `scene.setSetting`, so a divergence
        # here is a sky that disagrees with the Sun standing in front of it.
        for case, js in zip(self.cases, self.actual):
            state = solar.sun_state(
                ANCHOR_LON, case["lat"], case["when"], ALTITUDE, TURBIDITY, PEAK_INTENSITY
            )
            for channel, (got, expected) in enumerate(zip(js["ambient"], state.ambient)):
                self._close(got, expected, f"ambient[{channel}]", case)
            for channel, (got, expected) in enumerate(
                zip(js["horizonColor"], state.horizon_color)
            ):
                self._close(got, expected, f"horizon[{channel}]", case)
            self._close(js["iblIntensity"], state.ibl_intensity, "IBL intensity", case)

    def test_sky_photographs_match(self) -> None:
        # Which two skies, how far faded, how each is turned and how bright:
        # the runtime half of `r1/skies.py`, against the module itself.
        table = skies.load_table()
        for case, js in zip(self.cases, self.actual):
            state = solar.sun_state(
                ANCHOR_LON, case["lat"], case["when"], ALTITUDE, TURBIDITY, PEAK_INTENSITY
            )
            want = skies.sky_state(state, table)
            got = js["dome"]
            self.assertEqual(got["texture"], want.texture, case["when"].isoformat())
            self.assertEqual(got["blendTexture"], want.blend_texture, case["when"].isoformat())
            self._close(got["blend"], want.blend, "sky blend", case)
            self._close(got["rotation"], want.rotation, "sky rotation", case)
            self._close(got["blendRotation"], want.blend_rotation, "blend sky rotation", case)
            self._close(got["exposure"], want.exposure, "sky exposure", case)
            for axis in range(3):
                self._close(got["sunDirection"][axis], want.sun_direction[axis],
                            "sun disc direction", case)
                self._close(got["sunColor"][axis], want.sun_color[axis], "sun disc radiance", case)

    def test_night_is_dark_and_noon_is_bright(self) -> None:
        # A parity test alone would pass if both sides were wrong in the same
        # way, so one absolute anchor: the Sun is off below the horizon and the
        # beam is warmer than it is blue whenever it is low.
        for case, js in zip(self.cases, self.actual):
            if js["elevation"] < solar.SUNSET_ELEVATION - 2.0:
                self.assertEqual(js["intensity"], 0.0)
            if 0.0 < js["elevation"] < 5.0:
                self.assertGreater(js["color"][0], js["color"][2])


if __name__ == "__main__":
    unittest.main()


# The world mode moves the observer instead of anchoring it, so `setObserver`
# carries the whole model to the new place. The harness above builds the module
# with an alpine anchor baked into `props`, which is exactly the wrong state
# for this: what has to be proven is that after the call the script computes the
# *destination's* sun and keeps nothing of the anchor's. Five sites, chosen for
# what they break rather than for coverage of a map.
OBSERVER_HARNESS = r"""
const fs = require("fs");
const source = fs.readFileSync(process.argv[2], "utf8");
const cases = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
const out = [];
for (const c of cases) {
    const props = {};
    const noop = () => {};
    const node = { setProperty: () => true, setRotation: () => {} };
    const tree = { firstInGroup: () => null };
    new Function("exportProperty", "props", "node", "tree", source + "\n")(
        (name, value) => { props[name] = value; }, props, node, tree);
    const bound = Object.assign({}, props, c.anchor);
    const m = new Function(
        "exportProperty", "props", "node", "tree",
        source + "\nreturn { setObserver, sunAngles, sunLight, skyLight, airMass };\n"
    )(noop, bound, node, tree);
    const accepted = m.setObserver(c.lon, c.lat, c.altitude);
    const angles = m.sunAngles(c.unix);
    const light = m.sunLight(c.unix);
    out.push({
        accepted: accepted,
        azimuth: angles.azimuth,
        elevation: angles.elevation,
        airMass: m.airMass(angles.elevation, c.altitude),
        direction: light.direction,
        color: light.color,
        intensity: light.intensity,
        ambient: m.skyLight(angles.elevation).ambient,
        rejected: [
            m.setObserver(NaN, 0.0, 0.0),
            m.setObserver(0.0, 91.0, 0.0),
            m.setObserver(181.0, 0.0, 0.0),
        ],
        afterRejection: m.sunAngles(c.unix).elevation,
    });
}
process.stdout.write(JSON.stringify(out));
"""

# lon, lat, altitude. Sydney and Cape Town put the Sun in the northern half of
# the sky and invert the season against the anchor; Tokyo is nearly half a turn
# of longitude away, which is where an equation-of-time or hour-angle sign error
# shows; Quito removes the seasonal swing; Tromso keeps the polar branches in.
SITES = (
    (151.2093, -33.8688, 30.0),
    (18.4241, -33.9249, 25.0),
    (139.7671, 35.6812, 11.0),
    (-78.4678, -0.1807, 2850.0),
    (18.9553, 69.6492, 10.0),
)


class SunCycleObserverTests(unittest.TestCase):
    """`setObserver` moves the whole model, and refuses what would black it out."""

    @classmethod
    def setUpClass(cls) -> None:
        node = _node()
        if node is None:
            raise unittest.SkipTest("node is not installed; JS parity cannot be checked")
        when = datetime(2026, 6, 21, 9, 47, tzinfo=timezone.utc)
        cls.when = when
        cls.cases = [
            {
                "lon": lon, "lat": lat, "altitude": altitude,
                "unix": when.timestamp(),
                # The anchor stays put on purpose: anything that leaks
                # from it into the result is the bug this test exists for.
                "anchor": {
                    "anchorLon": ANCHOR_LON, "anchorLat": ANCHOR_LAT,
                    "epochUnix": when.timestamp(), "secondsPerSecond": 60.0,
                    "turbidity": TURBIDITY, "altitude": ALTITUDE,
                    "peakIntensity": PEAK_INTENSITY,
                },
            }
            for lon, lat, altitude in SITES
        ]
        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "observer.js"
            harness.write_text(OBSERVER_HARNESS, encoding="utf8")
            payload = Path(tmp) / "cases.json"
            payload.write_text(json.dumps(cls.cases), encoding="utf8")
            result = subprocess.run(
                [node, str(harness), str(SCRIPT), str(payload)],
                capture_output=True, text=True, check=False,
            )
        if result.returncode != 0:
            raise AssertionError(f"sun_cycle.js failed to run:\n{result.stderr}")
        cls.actual = json.loads(result.stdout)

    def _close(self, got: float, want: float, what: str, case: dict) -> None:
        scale = max(abs(want), 1.0)
        self.assertLessEqual(
            abs(got - want) / scale, TOLERANCE,
            f"{what} diverges at {case['lon']}, {case['lat']}: JS {got!r} vs Python {want!r}",
        )

    def test_the_sun_is_the_destination_s(self) -> None:
        for case, js in zip(self.cases, self.actual):
            self.assertTrue(js["accepted"])
            state = solar.sun_state(
                case["lon"], case["lat"], self.when,
                case["altitude"], TURBIDITY, PEAK_INTENSITY,
            )
            azimuth, elevation, _, _, _ = solar.sun_angles(case["lon"], case["lat"], self.when)
            self._close(js["azimuth"], azimuth, "azimuth", case)
            self._close(js["elevation"], elevation, "elevation", case)
            self._close(js["airMass"], solar.air_mass(elevation, case["altitude"]), "air mass", case)
            for axis, (got, expected) in enumerate(zip(js["direction"], state.direction)):
                self._close(got, expected, f"direction[{axis}]", case)
            for channel, (got, expected) in enumerate(zip(js["color"], state.color)):
                self._close(got, expected, f"color[{channel}]", case)
            self._close(js["intensity"], state.intensity, "intensity", case)
            for channel, (got, expected) in enumerate(zip(js["ambient"], state.ambient)):
                self._close(got, expected, f"ambient[{channel}]", case)

    def test_the_anchor_does_not_leak(self) -> None:
        # The cheapest way for this feature to be wrong and still look right is
        # for one term to keep reading the anchor. At the same instant the
        # anchor's elevation differs from every site's by more than a degree,
        # so a leak cannot hide inside the tolerance.
        _, anchor_elevation, _, _, _ = solar.sun_angles(ANCHOR_LON, ANCHOR_LAT, self.when)
        for case, js in zip(self.cases, self.actual):
            self.assertGreater(
                abs(js["elevation"] - anchor_elevation), 1.0,
                f"the sun at {case['lon']}, {case['lat']} is still the anchor's",
            )

    def test_a_bad_coordinate_is_refused_and_changes_nothing(self) -> None:
        # A NaN latitude would reach the light's direction and black the frame
        # out with nothing on screen to explain it, so the call must refuse and
        # leave the previous observer standing.
        for case, js in zip(self.cases, self.actual):
            self.assertEqual(js["rejected"], [False, False, False])
            self._close(js["afterRejection"], js["elevation"], "elevation after refusal", case)
