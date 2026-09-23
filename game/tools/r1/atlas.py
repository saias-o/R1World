"""Regional style profiles — the seed of the Atlas (plan §9).

The fidelity hierarchy (§2.1) says the first four ranks are *measured* and the
creative work only starts at rank 5. Every rank from there down — heights and
gabarits, facade materials and colours, roof form and pitch, vegetation, street
furniture — is inferred, and the Atlas is what infers it. It is named as the
central deliverable of the project, and it is a phase-4 chantier.

This module is not that Atlas. It is the shape the Atlas will have and the seam
every generator already calls through, holding thirty-four entries written by
hand where §9 asks for two hundred derived from statistics. Writing the lookup
this way means no generator ever hardcodes a storey height or a roof pitch, so
phase 4 replaces the body of `profile_for` and the tables under it, and touches
nothing else.

The entries come in three tiers and every profile says which it is, because the
difference matters to everyone downstream. A **region** is a place someone
looked at and described. A **band** is a continental-scale generalisation, right
about the broad strokes and silent about any particular street. **GENERIC**
knows nothing and says so. Bands are the compromise that keeps ninety-nine per
cent of the planet from being one grey box while the two hundred regions are
still unwritten, and labelling them is what keeps that compromise honest.

**What a profile is, and what it is not.** A profile carries the *statistics of
a place*: how tall a floor is, how wide a bay, which roof a building has when
nobody wrote it down, what colour the render is. It never carries a fact about a
specific building — anything measured comes from OSM and beats the profile every
time (§4 I5: the game always knows what it is guessing). A profile is therefore
only ever consulted as a fallback, and every value it supplies is recorded as
inferred in the build manifest.

**Determinism (I3).** Nothing here draws a random number. A profile offers
*distributions*; the caller samples them with a generator seeded from the
feature's OSM id, so the same building is the same building on every machine and
in every run.

The numbers below are not invented. Each comes from its own place's building
stock — Chamonix's, for instance, are 2.6-2.8 m floors, deep eaves that throw
snow clear of the walls, steep gables on the older chalets and shallower ones on
post-war construction, lime render in warm greys and creams over stone bases,
and slate or grey metal roofing rather than the terracotta of the south. They
are the kind of thing the Atlas will eventually derive from OSM statistics per
region; here they are stated, and stated in one place, so a wrong one is a
single edit.
"""

from __future__ import annotations

import random
from dataclasses import dataclass, field


@dataclass(frozen=True)
class Swatch:
    """One entry of a weighted palette: a name, a linear colour, a roughness."""

    name: str
    color: tuple[float, float, float]
    roughness: float
    weight: float = 1.0


def _pick(swatches: tuple[Swatch, ...], rng: random.Random) -> Swatch:
    """Weighted draw from a palette, from a caller-seeded generator.

    `random.choices` would do this in one line and is deliberately avoided: its
    implementation is free to change between Python versions, and a scene that
    regenerates differently under a new interpreter breaks the determinism
    contract (§4 I3) in the least visible way possible.
    """
    total = sum(s.weight for s in swatches)
    cursor = rng.random() * total
    for swatch in swatches:
        cursor -= swatch.weight
        if cursor <= 0.0:
            return swatch
    return swatches[-1]


@dataclass(frozen=True)
class RegionProfile:
    """Everything a generator needs to infer what the data does not say."""

    name: str
    # How much authority this profile has, and it is written into every build
    # manifest (§4 I5). "region" is a place someone looked at and described;
    # "band" is a continental-scale generalisation that is right about the
    # broad strokes and says nothing about any particular street; "none" is the
    # fallback, which knows nothing and does not pretend otherwise. A player
    # asking why a town looks the way it does deserves to be able to tell these
    # three apart, and so does whoever writes the next region.
    tier: str = "region"

    # ── massing ─────────────────────────────────────────────────────────────
    # Floor-to-floor height. OSM's `building:levels` is a count, so this is what
    # turns it into metres; it is also what a storey count is read back out of
    # when only a total height was tagged.
    storey_height: float = 2.75
    # A commercial ground floor is taller than the floors above it, everywhere.
    ground_storey_height: float = 3.6
    # Heights drawn when nothing at all is tagged, in storeys. The valley is
    # two- and three-storey with the occasional four; a flat mean would produce
    # a village of identical boxes, which is the failure mode this replaces.
    storey_weights: tuple[tuple[int, float], ...] = ((2, 4.0), (3, 3.0), (4, 1.0))

    # ── roofs (rank 7: inferred, and very discriminating from above) ────────
    roof_shape_weights: tuple[tuple[str, float], ...] = (
        ("gabled", 6.0), ("hipped", 3.0), ("flat", 1.0),
    )
    # Pitch in degrees. Alpine roofs are steep, but not uniformly so: the older
    # chalets are near 40°, post-war construction nearer 25°.
    roof_pitch_range: tuple[float, float] = (24.0, 40.0)
    # Eave overhang in metres. This one number does more for "this is a
    # mountain village" than any texture: deep eaves throw snow clear of the
    # walls and cast a hard shadow line along the top of every facade.
    eave_overhang: float = 0.85
    # Parapet height on a flat roof, so a flat roof reads as a roof and not as a
    # cut-off box.
    parapet_height: float = 0.45

    # ── facades (rank 11: synthesised, and worth it only if aligned) ────────
    # Target spacing between window axes. The generator divides each wall into a
    # whole number of bays nearest this, so bays are regular *per wall* rather
    # than globally — which is what real buildings do.
    bay_width: float = 3.1
    window_width: float = 1.15
    window_height: float = 1.45
    # Height of the sill above its own floor level.
    window_sill: float = 0.95
    # How deep the opening is recessed. A window flush with the render reads as
    # a decal; 12 cm of reveal is what makes it read as a hole.
    window_inset: float = 0.12
    # Minimum masonry between an opening and the edge of its bay, and between
    # an opening and the floor above. Below these the bay carries no window at
    # all rather than a cramped one.
    pier_min: float = 0.55
    lintel_min: float = 0.35
    # Shopfronts: wider, taller and closer to the pavement than a window.
    shopfront_width_ratio: float = 0.78
    shopfront_height: float = 2.35
    shopfront_sill: float = 0.35
    # A shopfront's piers are slimmer than a dwelling's: the whole point of a
    # ground-floor commercial front is that it is as open as the structure
    # allows. Sharing `pier_min` with the windows above would leave every
    # ground floor in the region blank.
    shopfront_pier_min: float = 0.28

    # ── materials (rank 6: the critical point, per §2.1) ────────────────────
    walls: tuple[Swatch, ...] = ()
    roofs: tuple[Swatch, ...] = ()
    trim: Swatch = Swatch("Trim", (0.86, 0.84, 0.79), 0.7)
    glass: Swatch = Swatch("Glazing", (0.09, 0.12, 0.15), 0.12)

    # ── ground (rank 9: measured where OSM mapped it, this where it did not) ─
    # What the earth is made of around here when nobody drew a polygon over it.
    # `ground.py` classifies what OSM *did* draw and only reaches for this where
    # it drew nothing, which in a mapped city is a few per cent of a tile and in
    # the Sahara is all of it. A temperate green is the default because most of
    # the inhabited, mapped world is temperate and green; every profile that is
    # not overrides it, and the ones that most need to are the arid bands and
    # the dense cities, where a lawn is not merely wrong but comic.
    #
    # Like `ground.py`'s table, these are *albedos* and not colours:
    # 0.10-0.15 for a made surface, 0.30-0.40 for dry sand, 0.08-0.15
    # under a forest. Chosen as colours instead, every one of them sits
    # above the point where a sunlit horizontal surface saturates, and
    # the whole distinction this field exists to make disappears into
    # white. That is not a hypothesis: it is what the first version of
    # this table did, measured at 230/255 for asphalt and 242/255 for
    # desert sand in the same frame.
    ground: Swatch = Swatch("Ground, temperate", (0.148, 0.196, 0.110), 0.94)

    # ── vegetation (rank 8: inferred by biome) ──────────────────────────────
    # Which species stands where OSM says "here is a tree" and nothing more,
    # which is what OSM almost always says. The tag carries a position and not a
    # genus, so the genus is the region's to answer, and it is rank 8 read
    # exactly as §2.1 writes it: "essences, densité, taille — inférée par
    # biome". Bare model stems; `props.py` knows where the files live.
    # A repeated entry is a heavier weight: the draw is uniform over the tuple,
    # so a temperate street that is mostly broadleaf says so by saying it twice.
    tree_models: tuple[str, ...] = ("broadleaf", "broadleaf", "fir_sapling")


    # A CC0 kit arrives painted in the kit's palette, not the region's. These
    # are what the normaliser (§11.4) repaints it to, so the species reads as
    # the species that actually grows here instead of as the asset it came
    # from — which is the difference between a valley and a kit.
    foliage: Swatch = Swatch("Conifer foliage", (0.10, 0.19, 0.11), 0.96)
    bark: Swatch = Swatch("Conifer bark", (0.20, 0.16, 0.13), 0.95)

    # Tags that make a ground floor commercial. Kept with the profile because
    # the vocabulary is regional in practice — what counts as a shopfront in a
    # French village is not what counts in a Tokyo block.
    commercial_keys: tuple[str, ...] = ("shop", "amenity", "office", "tourism")
    commercial_building_values: tuple[str, ...] = (
        "retail", "commercial", "hotel", "restaurant", "supermarket",
    )

    def wall_swatch(self, rng: random.Random) -> Swatch:
        return _pick(self.walls, rng)

    def roof_swatch(self, rng: random.Random) -> Swatch:
        return _pick(self.roofs, rng)

    def roof_shape(self, rng: random.Random) -> str:
        total = sum(weight for _, weight in self.roof_shape_weights)
        cursor = rng.random() * total
        for shape, weight in self.roof_shape_weights:
            cursor -= weight
            if cursor <= 0.0:
                return shape
        return self.roof_shape_weights[-1][0]

    def roof_pitch(self, rng: random.Random) -> float:
        low, high = self.roof_pitch_range
        return low + rng.random() * (high - low)

    def storeys(self, rng: random.Random) -> int:
        total = sum(weight for _, weight in self.storey_weights)
        cursor = rng.random() * total
        for count, weight in self.storey_weights:
            cursor -= weight
            if cursor <= 0.0:
                return count
        return self.storey_weights[-1][0]

    def is_commercial(self, tags: dict[str, str]) -> bool:
        if any(key in tags for key in self.commercial_keys):
            return True
        return tags.get("building", "") in self.commercial_building_values


# ── the palette shorthand ───────────────────────────────────────────────────

def _sw(name: str, color: tuple[float, float, float], roughness: float,
        weight: float = 1.0) -> Swatch:
    """`Swatch` under a shorter name, because the tables below are the content.

    Thirty profiles written out in full would bury the numbers that matter
    under the word `Swatch` repeated two hundred times.
    """
    return Swatch(name, color, roughness, weight)


# ── the regions written so far ──────────────────────────────────────────────
#
# A `tier="region"` profile is a place someone described: the numbers come from
# that place's own building stock, and a wrong one is a single edit here. There
# are twelve of them, which is twelve out of the two hundred §9 asks for. They
# are not a sample of the world — they are the places this project has actually
# walked in, plus the handful whose building culture is distinctive enough that
# a generic band would be visibly wrong (Tunis, Amsterdam, Manhattan).

CHAMONIX = RegionProfile(
    tree_models=("fir_sapling", "pine_sapling"),
    name="Alpes du Nord — vallée de Chamonix",
    walls=(
        _sw("Lime render, cream", (0.78, 0.73, 0.63), 0.90, 4.0),
        _sw("Lime render, warm grey", (0.68, 0.65, 0.60), 0.90, 3.0),
        _sw("Lime render, pale ochre", (0.80, 0.72, 0.56), 0.88, 2.0),
        _sw("Rendered stone, grey", (0.55, 0.53, 0.50), 0.94, 2.0),
        # Timber is a minority of the whole stock and a majority of what a
        # visitor photographs, which is why it is in the palette at all.
        _sw("Weathered larch", (0.35, 0.26, 0.18), 0.85, 1.5),
    ),
    roofs=(
        _sw("Slate, dark", (0.13, 0.14, 0.16), 0.72, 4.0),
        _sw("Grey standing seam", (0.24, 0.25, 0.27), 0.55, 3.0),
        _sw("Aged zinc", (0.31, 0.32, 0.33), 0.62, 2.0),
        _sw("Weathered shingle", (0.28, 0.24, 0.20), 0.88, 2.0),
    ),
)

# Haussmann's ordinance is the reason this region can be written at all: the
# cornice line, the storey count and the roof angle were *legislated*, so a
# guess made here is unusually likely to be right. Six storeys over a tall
# commercial ground floor, cream limestone, and a grey zinc roof — the mansard
# reads as a steep hip in this generator, which is wrong in the detail and
# right in the silhouette (§2.1: the silhouette is the rank that matters).
PARIS = RegionProfile(
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Paris intra-muros",
    storey_height=3.05,
    ground_storey_height=4.2,
    storey_weights=((4, 1.0), (5, 3.0), (6, 5.0), (7, 2.0)),
    roof_shape_weights=(("hipped", 6.0), ("flat", 2.5), ("gabled", 1.5)),
    roof_pitch_range=(38.0, 54.0),
    eave_overhang=0.28,
    parapet_height=0.75,
    bay_width=2.9,
    walls=(
        _sw("Pierre de taille, cream", (0.80, 0.76, 0.66), 0.88, 5.0),
        _sw("Pierre de taille, pale grey", (0.74, 0.72, 0.68), 0.88, 4.0),
        _sw("Plâtre, warm grey", (0.66, 0.63, 0.58), 0.90, 2.0),
        _sw("Brique, faded red", (0.48, 0.31, 0.26), 0.92, 1.0),
    ),
    roofs=(
        _sw("Zinc, grey", (0.30, 0.31, 0.33), 0.52, 6.0),
        _sw("Ardoise, blue-black", (0.12, 0.13, 0.16), 0.70, 3.0),
        _sw("Zinc, oxidised", (0.36, 0.37, 0.36), 0.60, 2.0),
    ),
)

# London reads as brick and slate at two to four storeys, and the terrace is
# the unit: party walls everywhere, which the generator already detects, and a
# shallow eave because a London roof stops at the verge of its own gable.
LONDON = RegionProfile(
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Londres — Grand Londres",
    storey_height=2.85,
    ground_storey_height=3.4,
    storey_weights=((2, 4.0), (3, 4.0), (4, 2.0), (5, 1.0)),
    roof_shape_weights=(("gabled", 6.0), ("hipped", 2.0), ("flat", 2.0)),
    roof_pitch_range=(33.0, 45.0),
    eave_overhang=0.18,
    parapet_height=0.55,
    walls=(
        _sw("London stock brick", (0.52, 0.44, 0.35), 0.93, 5.0),
        _sw("Red brick", (0.46, 0.28, 0.23), 0.93, 4.0),
        _sw("Stucco, off-white", (0.82, 0.80, 0.75), 0.85, 2.0),
        _sw("Portland stone", (0.76, 0.74, 0.69), 0.86, 1.0),
    ),
    roofs=(
        _sw("Welsh slate", (0.16, 0.17, 0.19), 0.72, 6.0),
        _sw("Clay tile, dark red", (0.32, 0.18, 0.14), 0.88, 2.0),
        _sw("Bitumen deck", (0.14, 0.14, 0.14), 0.90, 2.0),
    ),
)

# Amsterdam's canal belt is one of the few places where the *width* of a plot
# is the signature: narrow, tall, steeply gabled, and dark brick. A generic
# Low Countries band would put the same buildings there two storeys shorter.
AMSTERDAM = RegionProfile(
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Amsterdam — ceinture des canaux",
    storey_height=2.95,
    ground_storey_height=3.5,
    storey_weights=((3, 3.0), (4, 5.0), (5, 2.0)),
    roof_shape_weights=(("gabled", 7.0), ("hipped", 1.5), ("flat", 1.5)),
    roof_pitch_range=(45.0, 58.0),
    eave_overhang=0.12,
    parapet_height=0.5,
    bay_width=2.7,
    walls=(
        _sw("Dark brick", (0.28, 0.20, 0.17), 0.93, 5.0),
        _sw("Red brick", (0.44, 0.27, 0.22), 0.93, 3.0),
        _sw("Painted brick, cream", (0.78, 0.74, 0.66), 0.88, 1.5),
    ),
    roofs=(
        _sw("Dutch pantile, dark", (0.24, 0.17, 0.14), 0.85, 4.0),
        _sw("Slate, grey", (0.19, 0.20, 0.22), 0.72, 3.0),
    ),
)

# Barcelona's Eixample: a rigid grid of six- and seven-storey blocks, chamfered
# corners, ochre and pale render, flat roofs used as terraces. The parapet is
# tall because those roofs are inhabited.
BARCELONA = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "pine_sapling"),
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Barcelone — Eixample",
    storey_height=3.0,
    ground_storey_height=4.0,
    storey_weights=((5, 3.0), (6, 5.0), (7, 2.0)),
    roof_shape_weights=(("flat", 7.0), ("hipped", 2.0), ("gabled", 1.0)),
    roof_pitch_range=(16.0, 26.0),
    eave_overhang=0.45,
    parapet_height=0.95,
    walls=(
        _sw("Estuco, ochre", (0.78, 0.68, 0.48), 0.89, 4.0),
        _sw("Estuco, pale cream", (0.83, 0.79, 0.70), 0.88, 4.0),
        _sw("Estuco, terracotta wash", (0.72, 0.55, 0.44), 0.90, 2.0),
        _sw("Rendered grey", (0.64, 0.62, 0.59), 0.90, 1.5),
    ),
    roofs=(
        _sw("Terrace deck, pale", (0.60, 0.57, 0.52), 0.92, 5.0),
        _sw("Teja árabe", (0.55, 0.31, 0.20), 0.90, 3.0),
    ),
)

# Rome's centro storico is ochre and low-pitched terracotta, four to six
# storeys, and the roof colour is doing most of the work: the city reads warm
# from any hill, and nothing else in the palette does that.
ROME = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "pine_sapling"),
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Rome — centro storico",
    storey_height=3.1,
    ground_storey_height=4.0,
    storey_weights=((3, 2.0), (4, 4.0), (5, 3.0), (6, 1.0)),
    roof_shape_weights=(("hipped", 5.0), ("gabled", 2.0), ("flat", 3.0)),
    roof_pitch_range=(15.0, 25.0),
    eave_overhang=0.75,
    parapet_height=0.7,
    walls=(
        _sw("Intonaco, ochre", (0.79, 0.63, 0.40), 0.89, 5.0),
        _sw("Intonaco, sienna", (0.71, 0.48, 0.32), 0.90, 3.0),
        _sw("Intonaco, cream", (0.83, 0.78, 0.66), 0.88, 3.0),
        _sw("Travertine", (0.80, 0.77, 0.68), 0.85, 1.5),
    ),
    roofs=(
        _sw("Coppi, terracotta", (0.58, 0.31, 0.19), 0.90, 6.0),
        _sw("Coppi, weathered", (0.47, 0.30, 0.22), 0.92, 3.0),
    ),
)

# Istanbul: render over concrete frame, four to seven storeys, and a roof that
# is either a shallow tiled hip or a flat deck carrying tanks and dishes. The
# palette is deliberately desaturated — the city is grey-cream far more than it
# is the postcard's blue.
ISTANBUL = RegionProfile(
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Istanbul — rive européenne",
    storey_height=2.95,
    ground_storey_height=3.6,
    storey_weights=((4, 3.0), (5, 4.0), (6, 2.5), (7, 1.0)),
    roof_shape_weights=(("hipped", 4.0), ("flat", 4.0), ("gabled", 2.0)),
    roof_pitch_range=(14.0, 24.0),
    eave_overhang=0.65,
    parapet_height=0.7,
    walls=(
        _sw("Render, pale grey", (0.72, 0.71, 0.68), 0.90, 4.0),
        _sw("Render, cream", (0.80, 0.76, 0.66), 0.89, 3.0),
        _sw("Render, dusty rose", (0.72, 0.60, 0.55), 0.90, 2.0),
        _sw("Bare concrete", (0.58, 0.58, 0.56), 0.93, 1.5),
    ),
    roofs=(
        _sw("Clay tile, red", (0.53, 0.28, 0.18), 0.90, 5.0),
        _sw("Membrane deck, grey", (0.36, 0.36, 0.35), 0.92, 4.0),
    ),
)

# Tunis: the medina and the ville nouvelle share one thing, and it is the one
# thing that matters here — whitewash and a flat roof with a real parapet. Get
# either wrong and the city reads as southern Europe.
TUNIS = RegionProfile(
    tree_models=("quiver_tree", "quiver_tree_slim", "broadleaf"),
    ground=_sw("Ground, arid", (0.395, 0.352, 0.261), 0.94),
    name="Tunis — médina et ville nouvelle",
    storey_height=3.0,
    ground_storey_height=3.6,
    storey_weights=((2, 4.0), (3, 4.0), (4, 2.0), (5, 1.0)),
    roof_shape_weights=(("flat", 9.0), ("hipped", 1.0)),
    roof_pitch_range=(10.0, 18.0),
    eave_overhang=0.30,
    parapet_height=1.0,
    walls=(
        _sw("Whitewash", (0.88, 0.87, 0.84), 0.88, 6.0),
        _sw("Whitewash, sun-bleached", (0.84, 0.83, 0.79), 0.90, 3.0),
        _sw("Render, sand", (0.79, 0.72, 0.58), 0.90, 2.0),
        _sw("Render, pale ochre", (0.82, 0.74, 0.57), 0.89, 1.5),
    ),
    roofs=(
        _sw("Roof terrace, whitewashed", (0.80, 0.79, 0.75), 0.90, 6.0),
        _sw("Roof terrace, weathered", (0.66, 0.64, 0.59), 0.92, 3.0),
    ),
)

# Manhattan is the case the massing model would otherwise get worst: tall
# storeys, flat roofs, deep parapets, and nothing tagged below the famous
# towers. A generic North American band would put a two-storey clapboard house
# on Lexington Avenue.
MANHATTAN = RegionProfile(
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="New York — Manhattan",
    storey_height=3.35,
    ground_storey_height=4.6,
    storey_weights=((5, 2.0), (6, 4.0), (8, 3.0), (12, 1.5), (20, 0.5)),
    roof_shape_weights=(("flat", 9.5), ("hipped", 0.5)),
    roof_pitch_range=(10.0, 18.0),
    eave_overhang=0.20,
    parapet_height=1.1,
    bay_width=3.3,
    walls=(
        _sw("Red brick", (0.42, 0.26, 0.21), 0.92, 5.0),
        _sw("Buff brick", (0.62, 0.54, 0.43), 0.92, 3.0),
        _sw("Limestone", (0.74, 0.72, 0.67), 0.86, 2.0),
        _sw("Glass curtain wall", (0.22, 0.27, 0.31), 0.25, 1.5),
    ),
    roofs=(
        _sw("Tar and gravel", (0.22, 0.21, 0.20), 0.93, 6.0),
        _sw("Membrane, pale", (0.55, 0.55, 0.53), 0.90, 2.0),
    ),
)

# Tokyo: the tagged buildings are towers and the untagged ones are the
# three-storey concrete-and-panel stock that actually fills the frame. Storeys
# are short, roofs are flat or a single shallow slope, and the palette is
# almost colourless — which is the observation, not a shortcut.
TOKYO = RegionProfile(
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Tokyo — 23 arrondissements",
    storey_height=2.85,
    ground_storey_height=3.3,
    storey_weights=((2, 4.0), (3, 5.0), (4, 2.5), (5, 1.0), (8, 0.5)),
    roof_shape_weights=(("flat", 6.0), ("skillion", 2.0), ("hipped", 1.5), ("gabled", 0.5)),
    roof_pitch_range=(12.0, 26.0),
    eave_overhang=0.55,
    parapet_height=0.6,
    bay_width=2.6,
    walls=(
        _sw("Panel, off-white", (0.82, 0.81, 0.78), 0.86, 5.0),
        _sw("Panel, pale grey", (0.70, 0.70, 0.69), 0.87, 4.0),
        _sw("Tile cladding, beige", (0.68, 0.63, 0.55), 0.88, 2.0),
        _sw("Bare concrete", (0.56, 0.56, 0.55), 0.92, 2.0),
    ),
    roofs=(
        _sw("Waterproof deck, grey", (0.38, 0.38, 0.37), 0.92, 6.0),
        _sw("Kawara tile, blue-grey", (0.22, 0.24, 0.26), 0.80, 2.5),
        _sw("Galvanised sheet", (0.46, 0.47, 0.47), 0.55, 1.5),
    ),
)

# Sydney: single-storey brick under a hipped corrugated roof, with a deep eave
# that exists because of the sun rather than the snow. The eave is the tell.
SYDNEY = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "fir_sapling"),
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Sydney — bassin côtier",
    storey_height=2.85,
    ground_storey_height=3.4,
    storey_weights=((1, 4.0), (2, 4.0), (3, 1.5)),
    roof_shape_weights=(("hipped", 6.0), ("gabled", 2.5), ("flat", 1.5)),
    roof_pitch_range=(20.0, 30.0),
    eave_overhang=0.70,
    parapet_height=0.5,
    walls=(
        _sw("Face brick, red", (0.47, 0.29, 0.24), 0.92, 4.0),
        _sw("Face brick, cream", (0.72, 0.66, 0.55), 0.92, 3.0),
        _sw("Render, white", (0.84, 0.83, 0.80), 0.88, 2.5),
        _sw("Weatherboard, pale", (0.76, 0.75, 0.71), 0.87, 1.5),
    ),
    roofs=(
        _sw("Corrugated steel, charcoal", (0.20, 0.21, 0.22), 0.55, 4.0),
        _sw("Terracotta tile", (0.48, 0.27, 0.19), 0.90, 3.0),
        _sw("Corrugated steel, pale", (0.60, 0.61, 0.60), 0.50, 2.0),
    ),
)

# Cape Town: white and pastel render under corrugated metal or a flat deck, one
# to three storeys, with the Cape Dutch gable as the minority form that carries
# the recognition.
CAPE_TOWN = RegionProfile(
    ground=_sw("Made ground", (0.139, 0.135, 0.128), 0.93),
    name="Le Cap — City Bowl",
    storey_height=2.9,
    ground_storey_height=3.5,
    storey_weights=((1, 3.0), (2, 4.0), (3, 2.0), (4, 1.0)),
    roof_shape_weights=(("hipped", 4.0), ("flat", 3.5), ("gabled", 2.5)),
    roof_pitch_range=(18.0, 32.0),
    eave_overhang=0.55,
    parapet_height=0.65,
    walls=(
        _sw("Render, white", (0.86, 0.85, 0.82), 0.88, 5.0),
        _sw("Render, pastel blue", (0.66, 0.72, 0.75), 0.88, 2.0),
        _sw("Render, pastel ochre", (0.82, 0.73, 0.55), 0.88, 2.0),
        _sw("Face brick, red", (0.46, 0.29, 0.24), 0.92, 1.5),
    ),
    roofs=(
        _sw("Corrugated steel, dark green", (0.16, 0.22, 0.18), 0.60, 4.0),
        _sw("Corrugated steel, grey", (0.32, 0.33, 0.34), 0.58, 3.0),
        _sw("Flat deck, pale", (0.62, 0.61, 0.58), 0.92, 2.5),
    ),
)


# ── the bands ───────────────────────────────────────────────────────────────
#
# A `tier="band"` profile generalises at continental scale. It is right about
# the broad strokes — a Nordic town is painted timber under a steep roof, a
# Saharan one is flat-roofed earth render — and it says nothing whatever about
# any particular street. That is a real claim and a modest one, and the reason
# the tier is written into every manifest is so that nobody downstream has to
# guess which kind of statement produced a given building.
#
# Bands exist because the alternative is worse. With named regions and GENERIC
# alone, ninety-nine per cent of the planet would be the same characterless
# grey box, and risk n°1 of §14 — « le monde est vaste et ennuyeux » — would be
# met not by the Atlas but by the absence of one. A band is the smallest honest
# thing that can be said about a continent, and §9.2's similarity propagation is
# what eventually replaces it with something said about a place.

ALPINE = RegionProfile(
    tree_models=("fir_sapling", "pine_sapling"),
    name="Arc alpin", tier="band",
    storey_height=2.8, storey_weights=((2, 4.0), (3, 3.0), (4, 1.5)),
    roof_shape_weights=(("gabled", 6.0), ("hipped", 3.0), ("flat", 1.0)),
    roof_pitch_range=(26.0, 42.0), eave_overhang=0.80, parapet_height=0.45,
    walls=(_sw("Render, cream", (0.79, 0.74, 0.64), 0.90, 4.0),
           _sw("Render, warm grey", (0.68, 0.65, 0.60), 0.90, 3.0),
           _sw("Weathered timber", (0.36, 0.27, 0.19), 0.86, 2.0)),
    roofs=(_sw("Slate, dark", (0.14, 0.15, 0.17), 0.72, 4.0),
           _sw("Shingle, brown", (0.29, 0.25, 0.20), 0.88, 3.0),
           _sw("Grey sheet metal", (0.26, 0.27, 0.29), 0.58, 2.0)),
)

FRANCE = RegionProfile(
    name="France — hors Paris et arc alpin", tier="band",
    storey_height=2.85, ground_storey_height=3.6,
    storey_weights=((1, 2.0), (2, 5.0), (3, 3.0), (4, 1.0)),
    roof_shape_weights=(("gabled", 5.0), ("hipped", 3.5), ("flat", 1.5)),
    roof_pitch_range=(28.0, 42.0), eave_overhang=0.45, parapet_height=0.5,
    walls=(_sw("Enduit, cream", (0.80, 0.76, 0.66), 0.89, 4.0),
           _sw("Enduit, sand", (0.78, 0.71, 0.57), 0.89, 3.0),
           _sw("Enduit, pale grey", (0.70, 0.69, 0.66), 0.90, 2.0),
           _sw("Pierre calcaire", (0.75, 0.72, 0.64), 0.88, 1.5)),
    roofs=(_sw("Tuile, terracotta", (0.53, 0.30, 0.20), 0.90, 4.0),
           _sw("Ardoise", (0.15, 0.16, 0.18), 0.72, 3.0),
           _sw("Tuile, brown", (0.38, 0.26, 0.19), 0.90, 2.0)),
)

IBERIA = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "pine_sapling"),
    ground=_sw("Ground, dry grass", (0.236, 0.231, 0.136), 0.94),
    name="Péninsule ibérique", tier="band",
    storey_height=2.95, ground_storey_height=3.7,
    storey_weights=((2, 4.0), (3, 4.0), (4, 2.0), (5, 1.0)),
    roof_shape_weights=(("hipped", 4.0), ("flat", 3.5), ("gabled", 2.5)),
    roof_pitch_range=(15.0, 26.0), eave_overhang=0.60, parapet_height=0.75,
    walls=(_sw("Cal, white", (0.87, 0.86, 0.83), 0.88, 4.0),
           _sw("Estuco, ochre", (0.79, 0.69, 0.49), 0.89, 3.0),
           _sw("Estuco, cream", (0.83, 0.79, 0.70), 0.88, 3.0)),
    roofs=(_sw("Teja árabe", (0.56, 0.31, 0.20), 0.90, 6.0),
           _sw("Flat deck, pale", (0.62, 0.60, 0.55), 0.92, 2.0)),
)

ITALY = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "pine_sapling"),
    ground=_sw("Ground, dry grass", (0.222, 0.222, 0.132), 0.94),
    name="Italie", tier="band",
    storey_height=3.0, ground_storey_height=3.8,
    storey_weights=((2, 3.0), (3, 4.0), (4, 3.0), (5, 1.0)),
    roof_shape_weights=(("hipped", 5.0), ("gabled", 2.5), ("flat", 2.5)),
    roof_pitch_range=(14.0, 24.0), eave_overhang=0.75, parapet_height=0.65,
    walls=(_sw("Intonaco, ochre", (0.79, 0.64, 0.42), 0.89, 4.0),
           _sw("Intonaco, cream", (0.83, 0.78, 0.66), 0.88, 3.0),
           _sw("Intonaco, sienna", (0.71, 0.49, 0.33), 0.90, 2.0),
           _sw("Intonaco, pale grey", (0.71, 0.70, 0.67), 0.90, 1.5)),
    roofs=(_sw("Coppi, terracotta", (0.57, 0.31, 0.19), 0.90, 6.0),
           _sw("Coppi, weathered", (0.46, 0.30, 0.22), 0.92, 3.0)),
)

BALKANS_AEGEAN = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "pine_sapling"),
    ground=_sw("Ground, dry grass", (0.231, 0.226, 0.137), 0.94),
    name="Balkans et Égée", tier="band",
    storey_height=2.9, ground_storey_height=3.5,
    storey_weights=((2, 4.0), (3, 4.0), (4, 2.0)),
    roof_shape_weights=(("hipped", 4.0), ("flat", 3.0), ("gabled", 3.0)),
    roof_pitch_range=(16.0, 28.0), eave_overhang=0.65, parapet_height=0.7,
    walls=(_sw("Render, white", (0.87, 0.86, 0.84), 0.88, 5.0),
           _sw("Render, cream", (0.81, 0.77, 0.68), 0.89, 3.0),
           _sw("Render, pale grey", (0.70, 0.70, 0.68), 0.90, 2.0)),
    roofs=(_sw("Clay tile, red", (0.54, 0.29, 0.19), 0.90, 5.0),
           _sw("Flat deck, pale", (0.63, 0.62, 0.58), 0.92, 3.0)),
)

BRITISH_ISLES = RegionProfile(
    name="Îles Britanniques", tier="band",
    storey_height=2.8, ground_storey_height=3.3,
    storey_weights=((2, 5.0), (3, 3.0), (1, 1.5), (4, 1.0)),
    roof_shape_weights=(("gabled", 6.5), ("hipped", 2.0), ("flat", 1.5)),
    roof_pitch_range=(33.0, 45.0), eave_overhang=0.20, parapet_height=0.5,
    walls=(_sw("Stock brick", (0.53, 0.45, 0.36), 0.93, 4.0),
           _sw("Red brick", (0.46, 0.28, 0.23), 0.93, 4.0),
           _sw("Pebbledash, grey", (0.68, 0.67, 0.64), 0.94, 2.0),
           _sw("Render, white", (0.84, 0.83, 0.80), 0.88, 1.5)),
    roofs=(_sw("Slate, dark", (0.16, 0.17, 0.19), 0.72, 5.0),
           _sw("Concrete tile, grey", (0.34, 0.34, 0.33), 0.90, 3.0),
           _sw("Clay tile, red", (0.36, 0.20, 0.15), 0.89, 2.0)),
)

LOW_COUNTRIES = RegionProfile(
    name="Pays-Bas et Flandre", tier="band",
    storey_height=2.9, ground_storey_height=3.4,
    storey_weights=((2, 4.0), (3, 4.0), (4, 2.0)),
    roof_shape_weights=(("gabled", 6.5), ("hipped", 2.0), ("flat", 1.5)),
    roof_pitch_range=(40.0, 55.0), eave_overhang=0.15, parapet_height=0.5,
    walls=(_sw("Brick, dark", (0.31, 0.22, 0.19), 0.93, 4.0),
           _sw("Brick, red", (0.45, 0.28, 0.23), 0.93, 4.0),
           _sw("Brick, buff", (0.62, 0.55, 0.45), 0.93, 2.0)),
    roofs=(_sw("Pantile, dark", (0.25, 0.18, 0.15), 0.86, 4.0),
           _sw("Pantile, red", (0.44, 0.24, 0.17), 0.88, 3.0),
           _sw("Slate, grey", (0.20, 0.21, 0.23), 0.72, 2.0)),
)

CENTRAL_EUROPE = RegionProfile(
    name="Europe centrale et germanique", tier="band",
    storey_height=2.85, ground_storey_height=3.5,
    storey_weights=((2, 4.0), (3, 4.0), (4, 2.5), (5, 1.0)),
    roof_shape_weights=(("gabled", 6.0), ("hipped", 3.0), ("flat", 1.0)),
    roof_pitch_range=(32.0, 48.0), eave_overhang=0.55, parapet_height=0.5,
    walls=(_sw("Putz, cream", (0.81, 0.77, 0.67), 0.89, 4.0),
           _sw("Putz, pale yellow", (0.83, 0.78, 0.58), 0.89, 2.5),
           _sw("Putz, pale grey", (0.71, 0.70, 0.68), 0.90, 2.5),
           _sw("Brick, red", (0.46, 0.29, 0.24), 0.93, 1.5)),
    roofs=(_sw("Clay tile, red", (0.50, 0.27, 0.18), 0.89, 5.0),
           _sw("Clay tile, anthracite", (0.20, 0.20, 0.21), 0.88, 3.0),
           _sw("Slate", (0.16, 0.17, 0.19), 0.72, 2.0)),
)

NORDIC = RegionProfile(
    tree_models=("fir_sapling", "pine_sapling"),
    ground=_sw("Ground, boreal forest", (0.081, 0.119, 0.076), 0.95),
    name="Pays nordiques", tier="band",
    storey_height=2.8, ground_storey_height=3.3,
    storey_weights=((1, 2.5), (2, 5.0), (3, 2.5), (4, 1.0)),
    roof_shape_weights=(("gabled", 7.0), ("hipped", 2.0), ("flat", 1.0)),
    roof_pitch_range=(30.0, 45.0), eave_overhang=0.60, parapet_height=0.45,
    walls=(_sw("Falu red timber", (0.36, 0.14, 0.11), 0.90, 4.0),
           _sw("Painted timber, ochre", (0.72, 0.57, 0.32), 0.89, 2.5),
           _sw("Painted timber, white", (0.85, 0.84, 0.81), 0.87, 2.5),
           _sw("Render, pale grey", (0.71, 0.71, 0.69), 0.90, 2.0)),
    roofs=(_sw("Corrugated steel, dark", (0.19, 0.20, 0.21), 0.58, 4.0),
           _sw("Clay tile, red", (0.46, 0.25, 0.17), 0.89, 3.0),
           _sw("Bitumen shingle", (0.22, 0.22, 0.21), 0.92, 2.0)),
)

EASTERN_EUROPE = RegionProfile(
    name="Europe orientale", tier="band",
    storey_height=2.75, ground_storey_height=3.3,
    storey_weights=((2, 4.0), (3, 3.0), (5, 2.5), (9, 1.5)),
    roof_shape_weights=(("gabled", 4.0), ("flat", 4.0), ("hipped", 2.0)),
    roof_pitch_range=(24.0, 38.0), eave_overhang=0.45, parapet_height=0.6,
    walls=(_sw("Render, cream", (0.80, 0.76, 0.65), 0.90, 3.5),
           _sw("Render, pale grey", (0.70, 0.70, 0.68), 0.90, 3.5),
           _sw("Panel concrete", (0.62, 0.62, 0.60), 0.93, 2.5),
           _sw("Brick, red", (0.45, 0.29, 0.24), 0.93, 1.5)),
    roofs=(_sw("Sheet metal, grey", (0.32, 0.33, 0.34), 0.60, 4.0),
           _sw("Clay tile, red", (0.48, 0.27, 0.18), 0.89, 3.0),
           _sw("Bitumen deck", (0.18, 0.18, 0.18), 0.92, 2.5)),
)

RUSSIA_SIBERIA = RegionProfile(
    tree_models=("fir_sapling", "pine_sapling"),
    ground=_sw("Ground, taiga", (0.086, 0.119, 0.076), 0.95),
    name="Russie et Sibérie", tier="band",
    storey_height=2.75, ground_storey_height=3.2,
    storey_weights=((1, 2.5), (2, 3.0), (5, 3.0), (9, 1.5)),
    roof_shape_weights=(("gabled", 5.0), ("flat", 4.0), ("hipped", 1.0)),
    roof_pitch_range=(24.0, 38.0), eave_overhang=0.50, parapet_height=0.6,
    walls=(_sw("Render, pale yellow", (0.81, 0.76, 0.55), 0.90, 3.0),
           _sw("Panel concrete", (0.63, 0.63, 0.61), 0.93, 3.0),
           _sw("Timber, weathered", (0.38, 0.30, 0.22), 0.90, 2.5),
           _sw("Brick, buff", (0.62, 0.55, 0.45), 0.93, 1.5)),
    roofs=(_sw("Sheet metal, green", (0.18, 0.26, 0.20), 0.58, 3.5),
           _sw("Sheet metal, grey", (0.33, 0.34, 0.35), 0.58, 3.5),
           _sw("Bitumen deck", (0.18, 0.18, 0.18), 0.92, 2.0)),
)

MAGHREB_SAHARA = RegionProfile(
    tree_models=("quiver_tree", "quiver_tree_slim", "broadleaf"),
    ground=_sw("Ground, desert sand", (0.437, 0.393, 0.293), 0.94),
    name="Maghreb et Sahara", tier="band",
    storey_height=3.0, ground_storey_height=3.6,
    storey_weights=((1, 3.0), (2, 5.0), (3, 3.0), (4, 1.0)),
    roof_shape_weights=(("flat", 9.0), ("hipped", 1.0)),
    roof_pitch_range=(10.0, 18.0), eave_overhang=0.30, parapet_height=0.95,
    walls=(_sw("Render, sand", (0.80, 0.72, 0.56), 0.90, 4.0),
           _sw("Whitewash", (0.87, 0.86, 0.83), 0.88, 3.5),
           _sw("Render, ochre", (0.78, 0.65, 0.44), 0.90, 2.5),
           _sw("Earth render, pink", (0.74, 0.55, 0.42), 0.92, 2.0)),
    roofs=(_sw("Roof terrace, sand", (0.72, 0.66, 0.53), 0.92, 6.0),
           _sw("Roof terrace, whitewashed", (0.79, 0.78, 0.74), 0.90, 3.0)),
)

MIDDLE_EAST = RegionProfile(
    tree_models=("quiver_tree", "quiver_tree_slim", "broadleaf"),
    ground=_sw("Ground, stony arid", (0.329, 0.301, 0.235), 0.95),
    name="Proche et Moyen-Orient", tier="band",
    storey_height=3.05, ground_storey_height=3.7,
    storey_weights=((2, 4.0), (3, 4.0), (4, 2.5), (6, 1.0)),
    roof_shape_weights=(("flat", 9.0), ("hipped", 1.0)),
    roof_pitch_range=(10.0, 18.0), eave_overhang=0.35, parapet_height=0.95,
    walls=(_sw("Limestone, pale", (0.81, 0.76, 0.63), 0.89, 4.5),
           _sw("Render, sand", (0.79, 0.71, 0.55), 0.90, 3.0),
           _sw("Bare concrete", (0.60, 0.59, 0.57), 0.93, 2.0),
           _sw("Render, cream", (0.83, 0.79, 0.69), 0.89, 2.0)),
    roofs=(_sw("Roof terrace, sand", (0.71, 0.66, 0.54), 0.92, 6.0),
           _sw("Membrane deck, grey", (0.40, 0.40, 0.39), 0.92, 2.5)),
)

SUB_SAHARAN = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "quiver_tree"),
    ground=_sw("Ground, dry savanna", (0.283, 0.244, 0.142), 0.94),
    name="Afrique subsaharienne", tier="band",
    storey_height=2.9, ground_storey_height=3.4,
    storey_weights=((1, 5.0), (2, 3.5), (3, 1.5)),
    roof_shape_weights=(("hipped", 4.0), ("gabled", 3.5), ("flat", 2.5)),
    roof_pitch_range=(16.0, 28.0), eave_overhang=0.60, parapet_height=0.6,
    walls=(_sw("Render, sand", (0.79, 0.71, 0.55), 0.91, 4.0),
           _sw("Render, ochre", (0.77, 0.62, 0.41), 0.91, 3.0),
           _sw("Blockwork, grey", (0.63, 0.62, 0.59), 0.93, 2.5),
           _sw("Render, pale blue", (0.63, 0.71, 0.74), 0.90, 1.5)),
    roofs=(_sw("Corrugated steel, rusted", (0.40, 0.27, 0.19), 0.85, 4.5),
           _sw("Corrugated steel, grey", (0.44, 0.45, 0.45), 0.55, 3.5),
           _sw("Flat deck, sand", (0.68, 0.63, 0.52), 0.92, 2.0)),
)

SOUTH_ASIA = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "quiver_tree"),
    ground=_sw("Ground, cultivated", (0.213, 0.208, 0.117), 0.94),
    name="Asie du Sud", tier="band",
    storey_height=2.95, ground_storey_height=3.5,
    storey_weights=((1, 3.0), (2, 4.5), (3, 3.0), (4, 1.5)),
    roof_shape_weights=(("flat", 7.0), ("hipped", 1.5), ("skillion", 1.5)),
    roof_pitch_range=(12.0, 22.0), eave_overhang=0.45, parapet_height=0.9,
    walls=(_sw("Render, cream", (0.81, 0.77, 0.66), 0.90, 3.5),
           _sw("Render, pale green", (0.68, 0.74, 0.63), 0.90, 2.0),
           _sw("Render, ochre", (0.78, 0.65, 0.44), 0.90, 2.5),
           _sw("Bare concrete", (0.58, 0.58, 0.56), 0.93, 2.5)),
    roofs=(_sw("Concrete deck", (0.55, 0.54, 0.51), 0.93, 5.0),
           _sw("Corrugated steel, grey", (0.43, 0.44, 0.44), 0.58, 2.5),
           _sw("Clay tile, red", (0.51, 0.28, 0.18), 0.90, 2.0)),
)

SOUTHEAST_ASIA = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "quiver_tree"),
    ground=_sw("Ground, humid tropics", (0.098, 0.147, 0.074), 0.95),
    name="Asie du Sud-Est", tier="band",
    storey_height=3.0, ground_storey_height=3.6,
    storey_weights=((1, 3.0), (2, 4.5), (3, 3.0), (4, 1.5)),
    roof_shape_weights=(("gabled", 4.5), ("hipped", 3.0), ("flat", 2.5)),
    roof_pitch_range=(20.0, 34.0), eave_overhang=0.80, parapet_height=0.7,
    walls=(_sw("Render, white", (0.85, 0.84, 0.81), 0.89, 4.0),
           _sw("Render, pastel yellow", (0.82, 0.77, 0.56), 0.89, 2.5),
           _sw("Render, pale grey", (0.70, 0.70, 0.68), 0.90, 2.5),
           _sw("Bare concrete", (0.58, 0.58, 0.56), 0.93, 1.5)),
    roofs=(_sw("Corrugated steel, red", (0.45, 0.22, 0.16), 0.70, 4.0),
           _sw("Corrugated steel, blue", (0.20, 0.28, 0.40), 0.65, 3.0),
           _sw("Clay tile, terracotta", (0.53, 0.29, 0.19), 0.90, 2.5)),
)

EAST_ASIA = RegionProfile(
    ground=_sw("Ground, cultivated", (0.186, 0.202, 0.114), 0.94),
    name="Asie de l'Est continentale", tier="band",
    storey_height=2.9, ground_storey_height=3.5,
    storey_weights=((2, 3.0), (3, 3.5), (6, 3.0), (12, 1.5)),
    roof_shape_weights=(("flat", 6.5), ("hipped", 2.0), ("gabled", 1.5)),
    roof_pitch_range=(14.0, 26.0), eave_overhang=0.60, parapet_height=0.8,
    walls=(_sw("Tile cladding, cream", (0.78, 0.75, 0.67), 0.88, 4.0),
           _sw("Render, pale grey", (0.71, 0.71, 0.69), 0.90, 3.0),
           _sw("Bare concrete", (0.58, 0.58, 0.56), 0.93, 2.0),
           _sw("Tile cladding, pale blue", (0.64, 0.70, 0.74), 0.87, 1.5)),
    roofs=(_sw("Waterproof deck, grey", (0.38, 0.38, 0.37), 0.92, 5.5),
           _sw("Glazed tile, grey", (0.24, 0.25, 0.27), 0.78, 2.0),
           _sw("Sheet metal, blue", (0.19, 0.27, 0.38), 0.62, 1.5)),
)

JAPAN = RegionProfile(
    name="Japon", tier="band",
    storey_height=2.85, ground_storey_height=3.3,
    storey_weights=((1, 2.5), (2, 5.0), (3, 3.0), (4, 1.0)),
    roof_shape_weights=(("gabled", 3.5), ("hipped", 3.0), ("flat", 2.5), ("skillion", 1.0)),
    roof_pitch_range=(18.0, 30.0), eave_overhang=0.75, parapet_height=0.55,
    walls=(_sw("Panel, off-white", (0.82, 0.81, 0.78), 0.86, 4.5),
           _sw("Panel, pale grey", (0.70, 0.70, 0.69), 0.87, 3.0),
           _sw("Timber, dark", (0.30, 0.24, 0.19), 0.89, 2.0),
           _sw("Bare concrete", (0.57, 0.57, 0.56), 0.92, 1.5)),
    roofs=(_sw("Kawara tile, blue-grey", (0.22, 0.24, 0.26), 0.80, 5.0),
           _sw("Kawara tile, black", (0.13, 0.13, 0.14), 0.78, 2.5),
           _sw("Galvanised sheet", (0.46, 0.47, 0.47), 0.55, 2.0)),
)

OCEANIA = RegionProfile(
    ground=_sw("Ground, dry bush", (0.248, 0.227, 0.134), 0.94),
    name="Australie et Pacifique", tier="band",
    storey_height=2.85, ground_storey_height=3.4,
    storey_weights=((1, 5.0), (2, 3.5), (3, 1.5)),
    roof_shape_weights=(("hipped", 5.0), ("gabled", 3.0), ("flat", 2.0)),
    roof_pitch_range=(18.0, 30.0), eave_overhang=0.70, parapet_height=0.5,
    walls=(_sw("Face brick, red", (0.47, 0.29, 0.24), 0.92, 3.5),
           _sw("Weatherboard, pale", (0.77, 0.76, 0.72), 0.87, 3.0),
           _sw("Render, white", (0.85, 0.84, 0.81), 0.88, 2.5),
           _sw("Face brick, cream", (0.72, 0.66, 0.55), 0.92, 2.0)),
    roofs=(_sw("Corrugated steel, charcoal", (0.20, 0.21, 0.22), 0.55, 4.5),
           _sw("Corrugated steel, pale", (0.60, 0.61, 0.60), 0.50, 2.5),
           _sw("Concrete tile, brown", (0.36, 0.26, 0.20), 0.90, 2.0)),
)

NORTH_AMERICA = RegionProfile(
    name="Amérique du Nord", tier="band",
    storey_height=2.9, ground_storey_height=3.6,
    storey_weights=((1, 4.0), (2, 5.0), (3, 2.0), (4, 1.0)),
    roof_shape_weights=(("gabled", 5.5), ("hipped", 2.5), ("flat", 2.0)),
    roof_pitch_range=(22.0, 38.0), eave_overhang=0.45, parapet_height=0.7,
    walls=(_sw("Clapboard, pale grey", (0.72, 0.72, 0.70), 0.88, 3.5),
           _sw("Clapboard, white", (0.85, 0.84, 0.82), 0.87, 3.0),
           _sw("Face brick, red", (0.45, 0.28, 0.23), 0.92, 2.5),
           _sw("Vinyl siding, beige", (0.74, 0.69, 0.58), 0.88, 2.0)),
    roofs=(_sw("Asphalt shingle, charcoal", (0.19, 0.19, 0.19), 0.93, 5.5),
           _sw("Asphalt shingle, brown", (0.30, 0.24, 0.19), 0.93, 2.5),
           _sw("Membrane deck, pale", (0.55, 0.55, 0.53), 0.90, 2.0)),
)

LATIN_AMERICA = RegionProfile(
    tree_models=("broadleaf", "broadleaf", "quiver_tree"),
    ground=_sw("Ground, subtropical", (0.134, 0.180, 0.098), 0.94),
    name="Amérique latine", tier="band",
    storey_height=2.9, ground_storey_height=3.5,
    storey_weights=((1, 3.5), (2, 4.5), (3, 2.5), (5, 1.0)),
    roof_shape_weights=(("flat", 5.0), ("hipped", 3.0), ("gabled", 2.0)),
    roof_pitch_range=(14.0, 26.0), eave_overhang=0.55, parapet_height=0.85,
    walls=(_sw("Render, white", (0.85, 0.84, 0.81), 0.89, 3.5),
           _sw("Render, ochre", (0.79, 0.66, 0.44), 0.90, 3.0),
           _sw("Render, terracotta", (0.72, 0.48, 0.37), 0.90, 2.0),
           _sw("Render, pale blue", (0.63, 0.71, 0.75), 0.90, 2.0)),
    roofs=(_sw("Concrete deck", (0.55, 0.54, 0.51), 0.93, 4.5),
           _sw("Teja, terracotta", (0.55, 0.30, 0.19), 0.90, 3.5),
           _sw("Corrugated steel, grey", (0.44, 0.45, 0.45), 0.58, 2.0)),
)

# Somewhere to fall back to when even a band has nothing to say — the open
# ocean, the ice sheets, and the seams between rectangles. It is deliberately
# characterless: a region with no style is honest about knowing nothing, and a
# scene built from it is visibly generic rather than confidently wrong (I5).
GENERIC = RegionProfile(
    name="Générique — aucune région Atlas",
    tier="none",
    storey_height=3.0,
    roof_shape_weights=(("gabled", 3.0), ("flat", 3.0), ("hipped", 1.0)),
    roof_pitch_range=(20.0, 30.0),
    eave_overhang=0.35,
    walls=(
        _sw("Neutral render", (0.72, 0.70, 0.67), 0.9, 3.0),
        _sw("Neutral render, grey", (0.62, 0.61, 0.60), 0.9, 2.0),
    ),
    roofs=(
        _sw("Neutral roofing", (0.26, 0.26, 0.27), 0.75),
    ),
)


# ── the lookup ──────────────────────────────────────────────────────────────
#
# Bounding boxes are a placeholder for the Atlas's real region geometry, which
# will be polygons carrying statistics rather than hand-written rectangles. The
# rectangles are generous on purpose: a profile is a *fallback*, so being
# slightly wrong at the edge of a valley costs nothing measured.
#
# **Order is the disambiguation, and it is the only one.** Rectangles overlap —
# Sicily sits inside any box drawn around North Africa, the Alps sit inside
# France and inside Italy at once — and rather than carve them into a jigsaw no
# reader could check, the first match wins and the table is written
# most-specific first. Adding a region means putting it above the band it
# refines; adding a band means asking which bands it must not steal from.

_REGIONS: tuple[tuple[tuple[float, float, float, float], RegionProfile], ...] = (
    # Named places first, always: a region refines the band it sits in.
    ((2.22, 48.81, 2.47, 48.91), PARIS),
    ((6.55, 45.75, 7.15, 46.10), CHAMONIX),
    ((-0.51, 51.28, 0.33, 51.69), LONDON),
    ((4.72, 52.31, 5.05, 52.43), AMSTERDAM),
    ((2.05, 41.32, 2.25, 41.47), BARCELONA),
    ((12.38, 41.80, 12.62, 41.99), ROME),
    ((28.85, 40.95, 29.20, 41.15), ISTANBUL),
    ((10.05, 36.70, 10.35, 36.90), TUNIS),
    ((-74.03, 40.68, -73.90, 40.88), MANHATTAN),
    ((139.55, 35.52, 139.92, 35.82), TOKYO),
    ((150.85, -34.10, 151.35, -33.65), SYDNEY),
    ((18.30, -34.15, 18.75, -33.85), CAPE_TOWN),

    # Then the bands. The alpine arc comes before France and Italy because it
    # crosses both and is the stronger statement where it applies; Japan comes
    # before continental East Asia for the same reason.
    ((5.60, 44.00, 16.20, 48.20), ALPINE),
    ((-9.60, 35.90, 3.40, 43.90), IBERIA),
    ((6.50, 36.50, 18.70, 47.10), ITALY),
    ((-11.00, 49.70, 2.10, 61.00), BRITISH_ISLES),
    ((2.30, 50.60, 7.30, 53.70), LOW_COUNTRIES),
    ((-5.30, 42.20, 8.30, 51.20), FRANCE),
    ((5.80, 45.50, 19.50, 55.10), CENTRAL_EUROPE),
    # West to -25 so Iceland, the Faroes and southern Greenland land here
    # rather than in GENERIC: painted timber under a steep roof is a better
    # statement about all three than no statement at all. The British Isles
    # are already claimed above, which is what keeps this from taking them.
    ((-25.00, 54.50, 32.00, 71.50), NORDIC),
    ((18.50, 34.80, 30.50, 42.50), BALKANS_AEGEAN),
    ((14.00, 40.00, 41.00, 60.00), EASTERN_EUROPE),
    ((26.00, 41.00, 180.00, 78.00), RUSSIA_SIBERIA),
    ((129.00, 30.00, 146.50, 46.00), JAPAN),
    ((-17.50, 18.00, 37.00, 37.60), MAGHREB_SAHARA),
    ((32.00, 12.00, 63.50, 42.50), MIDDLE_EAST),
    ((-20.00, -35.50, 52.00, 18.50), SUB_SAHARAN),
    ((60.00, 5.00, 92.50, 37.50), SOUTH_ASIA),
    ((92.00, -11.00, 141.00, 24.00), SOUTHEAST_ASIA),
    ((73.00, 18.00, 148.00, 54.00), EAST_ASIA),
    ((110.00, -48.00, 180.00, -8.00), OCEANIA),
    ((-170.00, 23.50, -52.00, 72.00), NORTH_AMERICA),
    ((-118.00, -56.00, -34.00, 33.00), LATIN_AMERICA),
)


def profile_for(lon: float, lat: float) -> RegionProfile:
    """The style profile that governs inference at this point on the ellipsoid.

    Thirty-four entries are written today; §9 replaces the table above with the
    Atlas and this function's body with a real lookup. Callers already go
    through it, so that change reaches every generator at once and none of them
    has to move.
    """
    for (west, south, east, north), profile in _REGIONS:
        if west <= lon <= east and south <= lat <= north:
            return profile
    return GENERIC


def seeded(osm_id: int, salt: int) -> random.Random:
    """A generator tied to one feature and one purpose.

    Two draws for the same building must not be correlated — a building whose
    wall colour decided its roof pitch would produce visible stripes of identical
    houses — and the same draw must be identical on every machine, so nothing
    here may touch the global `random` state. `salt` separates the purposes;
    `osm_id` separates the buildings; there is no third source of entropy.
    """
    return random.Random((osm_id * 1000003) ^ salt)
