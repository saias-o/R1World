// Runtime half of the solar contract (plan §2.2). The generator lights the
// scene for one instant; this advances that instant and keeps the Sun where the
// ephemeris says it is and the colour the air column says it is, so the day
// actually turns instead of being a fixed vector under a fixed colour.
//
// It is a deliberate port of `tools/r1/solar.py`, not an approximation of it:
// same NOAA/Meeus terms, same refraction polynomial, same Kasten-Young air
// mass, same Beer-Lambert extinction, same two calibration constants. That is a
// claim a comment cannot keep, so `tools/r1/tests/test_sun_cycle_parity.py`
// runs this file against the Python module over a year of instants and five
// latitudes and holds the two to 1e-9. A divergence is a bug in one of them,
// never a tuning difference.
//
// What it drives. The beam is a `LightNode`, so `node.setProperty` carries its
// direction, colour and intensity; the sky around it is `SceneSettings`, so
// `scene.setSetting` carries the ambient, the fog and the clear colour. Both
// come from one instant and one atmosphere, so nothing here can drift out of
// step with the light: a dusk reddens the beam, cools the ambient, warms the
// horizon and dims the sky together, because they are readings of the same
// air column rather than separate curves.
//
// And the sky itself follows the hour. `r1/skies.py` measured a day of
// photographed skies — where the Sun stood in each, which way each faces, how
// bright each is at the horizon — and took their Suns out; this picks the two
// photographs the real Sun stands between, crossfades them (`scene.setSkybox`
// and `skyboxBlend`), turns each to face the real Sun, scales them so their
// horizon is the model's horizon colour (the fog), and draws one Sun disc where
// the beam comes from. The table below
// is that module's output, and a test holds the two equal.

const RAD = Math.PI / 180.0;
const DEG = 180.0 / Math.PI;

// Apparent elevation of the Sun's centre at sunrise and sunset: half the disc
// plus the standard horizon refraction.
const SUNSET_ELEVATION = -0.833;
const NAUTICAL_TWILIGHT = -12.0;
const ASTRONOMICAL_TWILIGHT = -18.0;

// Atmosphere. Every constant below is `solar.py`'s, under its name there.
const PRESSURE_SCALE_HEIGHT = 8435.0;
const WAVELENGTHS = [0.610, 0.550, 0.465];
const ANGSTROM_EXPONENT = 1.3;
const AMBIENT_PEAK = 0.20;
const HORIZON_PEAK = 0.70;

exportProperty("anchorLon", 2.3522);
exportProperty("anchorLat", 48.8566);
// The generator uses this instant for the scene's placeholder first frame.
// Fixed-time scenes can opt out of the system clock for reproducible captures.
exportProperty("epochUnix", 1782052200.0);
exportProperty("useSystemClock", true);
// 1.0 follows real UTC. A scene may deliberately accelerate its own clock.
exportProperty("secondsPerSecond", 1.0);
exportProperty("turbidity", 2.4);
exportProperty("altitude", 0.0);
// Engine light intensity for an overhead Sun through a clear column. The
// generator passes the same number it lit the scene with, so the first frame
// reproduces it rather than stepping to a second calibration.
exportProperty("peakIntensity", 4.6);

// Where the observer stands. These arrive as scene properties, but the world
// teleports across the planet and the Sun is a function of *where the player
// is*: an anchor frozen on the first place would light Sydney at Paris's hour,
// put the beam on the wrong side of the sky and invert the seasons. So the observer is state,
// not a constant, and `setObserver` below is how the game moves it. Every term
// that depends on the place -- the hour angle, the latitude, the length of the
// air column -- reads it at each refresh rather than at load.
let observerLon = props.anchorLon;
let observerLat = props.anchorLat;
let observerAltitude = props.altitude;
const EPOCH_UNIX = props.useSystemClock ? Date.now() / 1000.0 : props.epochUnix;
const TIME_SCALE = props.secondsPerSecond;
const TURBIDITY = props.turbidity;
const PEAK_INTENSITY = props.peakIntensity;
// Haze, as an extinction per metre, where no visibility is measured: a clear
// day's 60 km (Koschmieder, 3.912 / V), the clearest a measurement is taken
// at below. The reference photographs read clearer still: the Vercors at
// 8.7 km and Table Mountain at 17 km keep half their contrast, about 90 km.
// The haze is the horizon's colour, far brighter than a facade, so a little
// of it hides a lot: 24 km left the Statue of Liberty a ghost at 2.7 km. It
// was a 5 km city haze, which the engine never drew past a kilometre (its fog
// took any depth over 0.9999 for sky). Kept equal to
// `prepare_world.FOG_DENSITY`, which gives the scene its first frame.
const FOG_DENSITY = 0.0000652;
let weatherCloud = 0.0;
let weatherRain = 0.0;
let weatherFog = FOG_DENSITY;

// `visibility`, when the game passes one, is the measured meteorological
// visibility in metres (Open-Meteo), turned into an extinction the same way
// (Koschmieder), as measured: what is measured is not thinned to hide where
// the streamed world ends.
function setWeather(cloudFraction, precipitation, visibility) {
    if (!isFinite(cloudFraction) || !isFinite(precipitation)) return false;
    weatherRain = Math.max(0.0, precipitation);
    weatherCloud = Math.max(0.0, Math.min(1.0, Math.max(cloudFraction, weatherRain * 0.5)));
    weatherFog = isFinite(visibility) && visibility > 0.0
        ? 3.912 / Math.max(50.0, Math.min(visibility, 60000.0)) : FOG_DENSITY;
    return true;
}

// How much daylight there is, 0 at night to 1 by day: what the game dims its
// own particles by (falling and blowing snow), which the light does not reach.
function daylight() {
    return skyLight(sunLight(gameTime()).sun.elevation).daylight;
}

// Called from the game (ScriptBehaviour::callExport) whenever the player lands
// somewhere new or walks far enough for the difference to be visible. Refusing
// a non-finite or out-of-range coordinate matters more here than it looks: a
// NaN latitude would propagate into the light's direction and black the frame
// out, with nothing on screen to say why.
function setObserver(lon, lat, altitude) {
    if (!isFinite(lon) || !isFinite(lat) || !isFinite(altitude)) return false;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    observerLon = lon;
    observerLat = lat;
    observerAltitude = altitude;
    // Deliberately does not refresh here: staying a pure state change keeps the
    // function callable without the engine's `node` and `scene` globals, which
    // is what lets the parity test check it against solar.py. `onUpdate` writes
    // the new sun on the next frame, sixteen milliseconds later.
    return true;
}

let inspectionMode = false;
let inspectionUnix = EPOCH_UNIX;
// A capture holds the instant it started at, or the one it names (Unix
// seconds), so two runs photograph the same place in the same light.
function setInspectionMode(enabled, unixSeconds) {
    if (typeof enabled !== "boolean") return false;
    if (unixSeconds !== undefined && !Number.isFinite(unixSeconds)) return false;
    if (enabled && Number.isFinite(unixSeconds)) inspectionUnix = unixSeconds;
    else if (enabled && !inspectionMode) inspectionUnix = gameTime();
    inspectionMode = enabled;
    return true;
}
let clock = null;
let lastLabel = "";
// The pair of skies last handed to `scene.setSkybox`, so the textures are only
// swapped when the Sun crosses into another pair and not on every frame.
let skyPair = "";

// ── ephemerides ─────────────────────────────────────────────────────────────

// Unix seconds -> Julian century since J2000.0. The Unix epoch is JD 2440587.5,
// so this needs no calendar arithmetic at all.
function julianCentury(unixSeconds) {
    return (unixSeconds / 86400.0 + 2440587.5 - 2451545.0) / 36525.0;
}

function solarGeometry(t) {
    const meanLongitude = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360.0;
    const meanAnomaly = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);

    const m = meanAnomaly * RAD;
    const equationOfCentre =
        Math.sin(m) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
        Math.sin(2 * m) * (0.019993 - 0.000101 * t) +
        Math.sin(3 * m) * 0.000289;

    const omega = (125.04 - 1934.136 * t) * RAD;
    const apparentLongitude = meanLongitude + equationOfCentre - 0.00569 - 0.00478 * Math.sin(omega);

    const meanObliquity =
        23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const obliquity = (meanObliquity + 0.00256 * Math.cos(omega)) * RAD;

    const declination =
        Math.asin(Math.sin(obliquity) * Math.sin(apparentLongitude * RAD)) * DEG;

    const y = Math.tan(obliquity / 2.0) * Math.tan(obliquity / 2.0);
    const l0 = meanLongitude * RAD;
    const equationOfTime =
        4.0 * DEG * (
            y * Math.sin(2 * l0) -
            2.0 * eccentricity * Math.sin(m) +
            4.0 * eccentricity * y * Math.sin(m) * Math.cos(2 * l0) -
            0.5 * y * y * Math.sin(4 * l0) -
            1.25 * eccentricity * eccentricity * Math.sin(2 * m)
        );
    return { declination: declination, equationOfTime: equationOfTime };
}

function refraction(trueElevation) {
    if (trueElevation > 85.0) return 0.0;
    const tanE = Math.tan(trueElevation * RAD);
    let arcseconds;
    if (trueElevation > 5.0) {
        arcseconds = 58.1 / tanE - 0.07 / (tanE * tanE * tanE) +
            0.000086 / (tanE * tanE * tanE * tanE * tanE);
    } else if (trueElevation > -0.575) {
        const e = trueElevation;
        arcseconds = 1735.0 + e * (-518.2 + e * (103.4 + e * (-12.79 + e * 0.711)));
    } else {
        arcseconds = -20.772 / tanE;
    }
    return arcseconds / 3600.0;
}

function sunAngles(unixSeconds) {
    const geometry = solarGeometry(julianCentury(unixSeconds));
    const minutes = (((unixSeconds % 86400.0) + 86400.0) % 86400.0) / 60.0;
    const trueSolarTime =
        ((minutes + geometry.equationOfTime + 4.0 * observerLon) % 1440.0 + 1440.0) % 1440.0;
    const hourAngle = (trueSolarTime / 4.0 - 180.0) * RAD;

    const phi = observerLat * RAD;
    const delta = geometry.declination * RAD;
    let cosZenith =
        Math.sin(phi) * Math.sin(delta) + Math.cos(phi) * Math.cos(delta) * Math.cos(hourAngle);
    cosZenith = Math.max(-1.0, Math.min(1.0, cosZenith));
    const zenith = Math.acos(cosZenith);
    const trueElevation = 90.0 - zenith * DEG;

    let azimuth;
    if (Math.sin(zenith) < 1e-9) {
        azimuth = 180.0;
    } else {
        azimuth = Math.atan2(
            Math.sin(hourAngle),
            Math.cos(hourAngle) * Math.sin(phi) - Math.tan(delta) * Math.cos(phi)
        ) * DEG + 180.0;
    }
    return {
        azimuth: ((azimuth % 360.0) + 360.0) % 360.0,
        elevation: trueElevation + refraction(trueElevation),
        trueElevation: trueElevation,
        trueSolarTime: trueSolarTime
    };
}

// ── atmosphere ──────────────────────────────────────────────────────────────

// Kasten-Young (1989), scaled by the pressure ratio at altitude: 1.0 overhead,
// 2.0 at 30°, about 38 at the horizon, and finite there — which 1/sin(h) is not.
function airMass(elevation, altitude) {
    const h = Math.max(elevation, -2.0);
    const relative = 1.0 / (Math.sin(h * RAD) + 0.50572 * Math.pow(h + 6.07995, -1.6364));
    return relative * Math.exp(-Math.max(altitude, 0.0) / PRESSURE_SCALE_HEIGHT);
}

function rayleighOpticalDepth(wavelength) {
    const inverse = 1.0 / Math.pow(wavelength, 4);
    return 0.008569 * inverse *
        (1.0 + 0.0113 / Math.pow(wavelength, 2) + 0.00013 / Math.pow(wavelength, 4));
}

function aerosolOpticalDepth(wavelength, turbidity) {
    const beta = 0.025 * Math.max(turbidity - 1.0, 0.0);
    return beta * Math.pow(wavelength, -ANGSTROM_EXPONENT);
}

// The one Beer-Lambert line that is the whole of the sunset: overhead the three
// channels differ by a few percent and the light reads white; at one degree the
// blue channel has lost 99.9% and the red 90%, with no gradient keyed anywhere.
function transmittance(elevation, altitude, turbidity) {
    const m = airMass(elevation, altitude);
    const out = [];
    for (let i = 0; i < 3; i++) {
        const w = WAVELENGTHS[i];
        out.push(Math.exp(-(rayleighOpticalDepth(w) + aerosolOpticalDepth(w, turbidity)) * m));
    }
    return out;
}

function normalise(rgb) {
    const peak = Math.max(rgb[0], rgb[1], rgb[2]);
    if (peak <= 1e-9) return [0.0, 0.0, 0.0];
    return [rgb[0] / peak, rgb[1] / peak, rgb[2] / peak];
}

function smoothstep(edge0, edge1, x) {
    const t = Math.max(0.0, Math.min(1.0, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0 - 2.0 * t);
}

// `LightNode.direction` is the direction the light *travels*, so this points
// away from the Sun. Engine axes: x=east, y=up, z=-north (`geodesy.py`).
function travelDirection(azimuth, elevation) {
    const e = elevation * RAD;
    const a = azimuth * RAD;
    return [
        -Math.cos(e) * Math.sin(a),
        -Math.sin(e),
        Math.cos(e) * Math.cos(a)
    ];
}

// Everything the light node needs at one instant. The colour is what survives
// the column, the intensity is how much of the beam is left, and the fade over
// the last two degrees stands in for the horizon clutter that hides a real
// setting Sun early — the disc itself sets over half a degree and should not
// snap.
function sunLight(unixSeconds) {
    const sun = sunAngles(unixSeconds);
    const direct = transmittance(sun.elevation, observerAltitude, TURBIDITY);
    const visibility = smoothstep(SUNSET_ELEVATION - 2.0, SUNSET_ELEVATION + 2.0, sun.elevation);
    const luminance = 0.2126 * direct[0] + 0.7152 * direct[1] + 0.0722 * direct[2];
    return {
        sun: sun,
        direction: travelDirection(sun.azimuth, sun.trueElevation),
        color: normalise(direct),
        intensity: PEAK_INTENSITY * luminance * visibility * (1.0 - 0.92 * weatherCloud)
    };
}

// The sky half of the same atmosphere: what the beam lost is what the dome
// gained, read off the zenith column rather than the Sun's slant path — the
// overhead sky stays blue all day even when the beam has gone orange, and
// taking 1 - T along the slant would wash the ambient grey at exactly the hour
// a dawn sky is least grey.
function skyLight(elevation) {
    const sky = normalise(transmittance(90.0, observerAltitude, TURBIDITY).map(function (c) {
        return 1.0 - c;
    }));
    const daylight = smoothstep(ASTRONOMICAL_TWILIGHT, 6.0, elevation);
    const nightAmbient = [0.001, 0.0015, 0.0025];
    const ambientPeak = AMBIENT_PEAK * (0.83 * smoothstep(-8.0, 25.0, elevation) + 0.17);

    const beam = smoothstep(NAUTICAL_TWILIGHT, 1.0, elevation);
    const warmth = (1.0 - smoothstep(0.0, 22.0, elevation)) * beam;
    const pale = sky.map(function (s) { return 0.58 + 0.42 * s; });
    const warm = normalise(transmittance(elevation, observerAltitude, TURBIDITY));
    const nightHorizon = [0.0015, 0.002, 0.004];

    const ambient = [];
    const horizon = [];
    for (let i = 0; i < 3; i++) {
        ambient.push(nightAmbient[i] + (ambientPeak * sky[i] - nightAmbient[i]) * daylight);
        horizon.push(nightHorizon[i] +
            (HORIZON_PEAK * (pale[i] + (warm[i] - pale[i]) * warmth) - nightHorizon[i]) * daylight);
    }
    const cloud = weatherCloud * daylight;
    const ambientGrey = 0.85 * (0.2126 * ambient[0] + 0.7152 * ambient[1] + 0.0722 * ambient[2]);
    const horizonGrey = 0.78 * (0.2126 * horizon[0] + 0.7152 * horizon[1] + 0.0722 * horizon[2]);
    for (let i = 0; i < 3; i++) {
        ambient[i] = ambient[i] * (1.0 - cloud) + ambientGrey * cloud;
        horizon[i] = horizon[i] * (1.0 - cloud) + horizonGrey * cloud;
    }
    // IBL is a correction to a stand-in, not a measurement:
    // `solar.py::hdri_daylight` carries the reasoning and this is its port.
    const hdri = Math.max(0.0, Math.min(1.0, (elevation + 6.0) / 12.0));
    return {
        ambient: ambient,
        horizonColor: horizon,
        daylight: daylight,
        iblIntensity: 0.12 + 0.88 * hdri
    };
}

// ── the sky photographs ─────────────────────────────────────────────────────

// BEGIN SKIES — generated by `python -m r1.skies`, do not edit by hand.
const SKIES = {
    "sunDiscRadiance": 20000.0,
    "sunAngularRadius": 0.265,
    "frames": [
        {
            "name": "night",
            "texture": "assets/skies/qwantani_night.hdr",
            "branch": "both",
            "elevation": -13.3082,
            "sunU": 0.62256,
            "horizon": 2.1216
        },
        {
            "name": "dawn",
            "texture": "assets/skies/qwantani_dawn.hdr",
            "branch": "rising",
            "elevation": -1.3802,
            "sunU": 0.59717,
            "horizon": 1.20562
        },
        {
            "name": "sunrise",
            "texture": "assets/skies/qwantani_sunrise.hdr",
            "branch": "rising",
            "elevation": 1.9177,
            "sunU": 0.59912,
            "horizon": 1.28874
        },
        {
            "name": "morning",
            "texture": "assets/skies/qwantani_morning.hdr",
            "branch": "rising",
            "elevation": 19.5373,
            "sunU": 0.6001,
            "horizon": 0.92853
        },
        {
            "name": "mid_morning",
            "texture": "assets/skies/qwantani_mid_morning.hdr",
            "branch": "rising",
            "elevation": 39.7005,
            "sunU": 0.6001,
            "horizon": 0.48062
        },
        {
            "name": "noon",
            "texture": "assets/skies/qwantani_noon.hdr",
            "branch": "both",
            "elevation": 49.5685,
            "sunU": 0.6001,
            "horizon": 0.34998
        },
        {
            "name": "afternoon",
            "texture": "assets/skies/qwantani_afternoon.hdr",
            "branch": "setting",
            "elevation": 40.7553,
            "sunU": 0.6001,
            "horizon": 0.45183
        },
        {
            "name": "late_afternoon",
            "texture": "assets/skies/qwantani_late_afternoon.hdr",
            "branch": "setting",
            "elevation": 19.5957,
            "sunU": 0.6001,
            "horizon": 0.81737
        },
        {
            "name": "sunset",
            "texture": "assets/skies/qwantani_sunset.hdr",
            "branch": "setting",
            "elevation": 6.5274,
            "sunU": 0.6001,
            "horizon": 1.82757
        },
        {
            "name": "dusk_1",
            "texture": "assets/skies/qwantani_dusk_1.hdr",
            "branch": "setting",
            "elevation": -0.9175,
            "sunU": 0.59619,
            "horizon": 0.69201
        },
        {
            "name": "dusk_2",
            "texture": "assets/skies/qwantani_dusk_2.hdr",
            "branch": "setting",
            "elevation": -5.2245,
            "sunU": 0.604,
            "horizon": 0.75773
        }
    ]
};
// END SKIES

// One branch's skies, lowest Sun first: morning skies on the way up, evening
// skies on the way down, night and noon on both. `r1/skies.py::branch_frames`.
function branchFrames(rising) {
    const out = [];
    for (let i = 0; i < SKIES.frames.length; i++) {
        const branch = SKIES.frames[i].branch;
        if (branch === "both" || branch === (rising ? "rising" : "setting")) out.push(i);
    }
    out.sort(function (a, b) { return SKIES.frames[a].elevation - SKIES.frames[b].elevation; });
    return out;
}

// The two skies the Sun stands between, and how far the first has faded into
// the second. Morning is the eastern half of the sky: before local noon the
// azimuth lies between 0° and 180° at every latitude. `select_skies`.
function selectSkies(elevation, azimuth) {
    const order = branchFrames(azimuth > 0.0 && azimuth < 180.0);
    const frames = SKIES.frames;
    if (elevation <= frames[order[0]].elevation) return { a: order[0], b: order[0], blend: 0.0 };
    for (let i = 0; i + 1 < order.length; i++) {
        const low = order[i];
        const high = order[i + 1];
        if (elevation < frames[high].elevation) {
            const t = (elevation - frames[low].elevation) / (frames[high].elevation - frames[low].elevation);
            return { a: low, b: high, blend: t * t * (3.0 - 2.0 * t) };
        }
    }
    const last = order[order.length - 1];
    return { a: last, b: last, blend: 0.0 };
}

// Radians turning a sky so its photographed Sun faces `azimuth`. `sky_rotation`.
function skyRotation(frame, azimuth) {
    const a = azimuth * RAD;
    const realU = Math.atan2(-Math.cos(a), Math.sin(a)) / (2.0 * Math.PI) + 0.5;
    const turn = (realU - frame.sunU) * 2.0 * Math.PI;
    const full = 2.0 * Math.PI;
    return ((turn % full) + full) % full;
}

// Everything the skybox needs at one instant: the photographs' horizon band is
// scaled to the model's horizon colour, which is also the fog, so the two meet
// without a seam. `r1/skies.py::sky_state`.
function skyState(light, horizon) {
    const sun = light.sun;
    const pick = selectSkies(sun.elevation, sun.azimuth);
    const a = SKIES.frames[pick.a];
    const b = SKIES.frames[pick.b];
    const cloud = weatherCloud * smoothstep(-8.0, 4.0, sun.elevation);
    const base = pick.blend < 0.5 ? a : b;
    const baseBand = cloud > 0.0 ? base.horizon : a.horizon * (1.0 - pick.blend) + b.horizon * pick.blend;
    const band = baseBand * (1.0 - cloud) + 0.75632 * cloud;
    const radiance = SKIES.sunDiscRadiance * light.intensity / PEAK_INTENSITY;
    return {
        texture: cloud > 0.0 ? base.texture : a.texture,
        blendTexture: cloud > 0.0 ? "assets/skies/overcast_soil_puresky.hdr" : b.texture,
        blend: cloud > 0.0 ? cloud : pick.blend,
        rotation: skyRotation(cloud > 0.0 ? base : a, sun.azimuth),
        blendRotation: cloud > 0.0 ? 0.0 : skyRotation(b, sun.azimuth),
        exposure: (0.2126 * horizon[0] + 0.7152 * horizon[1] + 0.0722 * horizon[2]) / band,
        sunDirection: [-light.direction[0], -light.direction[1], -light.direction[2]],
        sunColor: [radiance * (1.0 - cloud) * light.color[0],
                   radiance * (1.0 - cloud) * light.color[1],
                   radiance * (1.0 - cloud) * light.color[2]]
    };
}

// ── driving the light ───────────────────────────────────────────────────────

function formatSolarTime(minutes) {
    const hours = Math.floor(minutes / 60.0);
    const rest = Math.floor(minutes - hours * 60.0);
    return (hours < 10 ? "0" : "") + hours + ":" + (rest < 10 ? "0" : "") + rest;
}

function compass(azimuth) {
    const points = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"];
    return points[Math.round(azimuth / 22.5) % 16];
}

// About fifteen reflected writes a frame. Each is a name lookup and a small JSON
// conversion, which is nothing next to a draw call, and writing them all
// unconditionally is worth more than the state a change threshold would need:
// at 60x every one of them moves visibly within a few frames at dusk, and a
// threshold that held one back while the others advanced would put the beam and
// the sky it sits in at different hours.
// The instant the world is at, in Unix seconds. The game asks for it to tell
// the sea's traffic which day and hour it is (`r1/sea_traffic.py`), so the
// ships and the Sun cannot disagree about the time.
function gameTime() {
    if (inspectionMode) return inspectionUnix;
    return EPOCH_UNIX + (Date.now() / 1000.0 - EPOCH_UNIX) * TIME_SCALE;
}

function refreshSun() {
    const light = sunLight(gameTime());
    node.setProperty("direction", light.direction);
    node.setProperty("color", light.color);
    node.setProperty("intensity", light.intensity);

    const sky = skyLight(light.sun.elevation);
    scene.setSetting("ambientLight", sky.ambient);
    // The horizon band is both the fog and what shows behind the sky: one
    // colour, because they are one scattering column.
    scene.setSetting("fogColor", sky.horizonColor);
    scene.setSetting("clearColor", sky.horizonColor);
    scene.setSetting("fogDensity", weatherFog + Math.min(weatherRain, 2.0) * 0.001);
    scene.setSetting("iblDiffuseIntensity", sky.iblIntensity);
    scene.setSetting("iblSpecularIntensity", sky.iblIntensity);

    const dome = skyState(light, sky.horizonColor);
    const pair = dome.texture + "|" + dome.blendTexture;
    if (pair !== skyPair) {
        // The engine logs a refusal (a missing file) itself; remembering the
        // pair either way keeps a missing sky to one line, not one a frame.
        scene.setSkybox(dome.texture, dome.blendTexture);
        skyPair = pair;
    }
    scene.setSetting("skyboxBlend", dome.blend);
    scene.setSetting("skyboxRotation", dome.rotation);
    scene.setSetting("skyboxBlendRotation", dome.blendRotation);
    scene.setSetting("skyboxExposure", dome.exposure);
    scene.setSetting("skySunDirection", dome.sunDirection);
    scene.setSetting("skySunColor", dome.sunColor);

    if (clock === null || !clock.valid()) return;
    const sun = light.sun;
    const label =
        "Solar " + formatSolarTime(sun.trueSolarTime) +
        "  |  alt " + sun.elevation.toFixed(1) + "°" +
        "  az " + sun.azimuth.toFixed(1) + "° " + compass(sun.azimuth) +
        "  |  " + (sun.elevation > SUNSET_ELEVATION
            ? "I " + light.intensity.toFixed(2) + "  rgb " +
              light.color[0].toFixed(2) + "/" + light.color[1].toFixed(2) + "/" +
              light.color[2].toFixed(2)
            : "below horizon");
    if (label !== lastLabel) {
        clock.setText(label);
        lastLabel = label;
    }
}

function onReady() {
    clock = tree.firstInGroup("solarClock");
    scene.setSetting("skySunSize", SKIES.sunAngularRadius);
    refreshSun();
}

function onUpdate(deltaSeconds) {
    refreshSun();
}
