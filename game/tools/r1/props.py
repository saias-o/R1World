"""Street furniture and vegetation — rank 8 and rank 10, at almost no cost.

§2.1 puts vegetation at rank 8 and *mobilier urbain régional* at rank 10, and
calls the second one a strong cultural signature. Both had been entirely absent
from the world: OSM has been handing this generator a `natural=tree` node since
its very first query, and not one of them was ever planted.

**Why this is affordable, and it is the whole design.** Saida's `MeshCache` keys
meshes by `AssetID`, so a scene with six hundred nodes all pointing at
`broadleaf.glb` uploads that tree *once*: measured on one alpine valley,
sixty-four trees produced one hundred and twenty-eight primitive loads and twenty-one
distinct mesh ids for the entire scene. The vertex arena — the thing that
decides everything else in this project (see `native/world.cpp`) — therefore
charges for each prop *kind* and not for each prop.

The trees are the project's photoscanned Poly Haven ones, decimated to fit the
arena (see `decimate.py`), and they are never a kit's: rule 1 of `CLAUDE.md`
exists because that substitution was made here once. Eleven street-furniture
kinds cost 9 390 vertices and five tree species cost 77 639, once, for the
whole planet.

What a prop does cost is a node, and nodes are drawn. §12.2 budgets 400 k
triangles for "mobilier et détail L5" and §12.4 caps L5 at 200 m on the
reference machine, so the count is capped per tile rather than left to OSM —
Paris maps about twelve hundred placeable points per tile and Tunis maps three,
and neither number should be the one that decides the frame rate.

**The budget is shared out, not raced for.** Filling it strictly by priority
would give a Paris tile six hundred trees and not one bench, because there are
more trees. Each kind gets a share of the tile's budget instead, and whatever a
kind does not use falls through to the next: a street keeps its lamps *and* its
trees, and a village with forty mapped features gets all forty.

**Nothing here is invented.** Every prop stands on a point somebody surveyed;
there is no scattering, no density model, no "a village would have benches".
Where OSM is thin the world is thin, and the manifest says by how much (§4 I5).
That is the same bargain the buildings make, and it is why Tunis will look
emptier than Amsterdam until somebody maps Tunis.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path

from .atlas import RegionProfile, seeded
from .external_assets import prop_model, tree_model
from .mesh import model_height

# Salts, per `atlas.seeded`: a prop's model must not be correlated with its
# angle, or a street of trees all faces the same way and reads as wallpaper.
SALT_MODEL = 0x4D4F444C
SALT_YAW = 0x59415721
SALT_SCALE = 0x53434C45

# How many props one tile may carry. A tile is about 555 m across and holds nine
# of its kind in a neighbourhood, so this is ~2 200 nodes around the player.
# It is a draw-call budget, not a memory one: see the module docstring.
TILE_PROP_BUDGET = 260

# A prop further than this from a road keeps a seeded angle instead of facing
# it. Twenty-five metres is about as far as a bench can be from the path it
# belongs to before the path stops being the reason it is there.
ROAD_SNAP = 25.0


@dataclass(frozen=True)
class PropKind:
    """One thing that can stand on the ground, and how big it really is."""

    name: str
    models: tuple[str, ...]
    # The real object's height in metres. The model's own height is measured at
    # generation time and divided out, so a kit that re-exports at another scale
    # changes nothing here — and a street lamp is five metres tall because
    # street lamps are, not because a scale factor was tuned until it looked
    # right.
    height: float
    # Share of the tile budget this kind may claim before the others get a turn.
    share: float
    # Lower goes first, both for the budget and for the leftovers.
    priority: int
    # Face the nearest road when there is one. True for anything that exists
    # because of the street; false for anything that merely stands near it.
    faces_road: bool = False
    # ± fraction of `height`, drawn per feature. Zero for manufactured objects:
    # lamp posts on one street are the same lamp post, and jittering them is the
    # tell that says a generator was here.
    jitter: float = 0.0
    selectors: tuple[tuple[str, frozenset[str]], ...] = ()


def _tags(*pairs: tuple[str, str]) -> tuple[tuple[str, frozenset[str]], ...]:
    grouped: dict[str, set[str]] = {}
    for key, value in pairs:
        grouped.setdefault(key, set()).add(value)
    return tuple((key, frozenset(values)) for key, values in grouped.items())


# Read in order; the first kind whose selector matches wins. The shares add up
# to more than one on purpose — they are ceilings, and a tile that has only
# benches should be allowed to spend its whole budget on benches.
PROP_KINDS: tuple[PropKind, ...] = (
    # **Trees are off, and the share is zero rather than the kind deleted.**
    #
    # The project's trees are photoscanned (Poly Haven) and nothing may replace
    # them with a kit's — that is rule 1 of `CLAUDE.md` and it was broken here
    # once. `decimate.py` was written to bring them into the arena and it works,
    # but the measurement it produced is the reason this share is zero:
    #
    #   island_tree_01   1 303 928 vertices, of which 927 528 are canopy
    #   streamable size  about 12 000 vertices and 500 kB per species
    #   canopy survivor  0.43% -- roughly a hundred leaf clusters out of 25 000
    #
    # A hundred clusters is not a canopy. The tree keeps its trunk, its bark and
    # its proportions and loses everything that made it read as a plant, and a
    # street of skeletons is not an improvement on a street of nothing. Raising
    # the budget to where the canopy survives — about 185 000 vertices, a 6 MB
    # file — costs 15% of the arena per species and thirty-six seconds a tile,
    # because Saida re-parses a referenced file once per node (see
    # `decimate._shrink_texture`).
    #
    # The plan already knew: §12.3 lists impostors as lever 3, "un arbre à 200 m
    # devient deux triangles… la différence entre praticable et impraticable",
    # and §12.2 gives vegetation the largest triangle budget of any item. This
    # is M5, and it is the thing that unlocks rank 8 for good.
    #
    # Everything needed is in place and waiting for it: the species table per
    # biome in the Atlas, the placement, the budget, the decimator. Restoring
    # trees is this one number.
    PropKind(
        "tree", (), height=9.0, share=0.0, priority=0, jitter=0.30,
        selectors=_tags(("natural", "tree")),
    ),
    PropKind(
        "street lamp", (prop_model("street_lamp"), prop_model("street_lamp_curved")),
        height=5.0, share=0.28, priority=1, faces_road=True,
        selectors=_tags(("highway", "street_lamp")),
    ),
    PropKind(
        "bench", (prop_model("bench"),),
        height=0.85, share=0.12, priority=2, faces_road=True,
        selectors=_tags(("amenity", "bench")),
    ),
    # A bus stop is a pole with a sign far more often than it is a shelter, in
    # OSM and on the ground both, and no CC0 kit has a shelter. The pole is the
    # honest object rather than the flattering one.
    PropKind(
        "bus stop", (prop_model("bus_stop_sign"),),
        height=2.6, share=0.06, priority=3, faces_road=True,
        selectors=_tags(("highway", "bus_stop")),
    ),
    PropKind(
        "fountain", (prop_model("fountain"),),
        height=1.1, share=0.03, priority=4,
        selectors=_tags(("amenity", "fountain")),
    ),
    PropKind(
        "post box", (prop_model("post_box"),),
        height=1.4, share=0.04, priority=5, faces_road=True,
        selectors=_tags(("amenity", "post_box"), ("amenity", "telephone"),
                        ("amenity", "clock"), ("amenity", "drinking_water")),
    ),
    PropKind(
        "waste bin", (prop_model("waste_bin"),),
        height=1.2, share=0.06, priority=6, faces_road=True,
        selectors=_tags(("amenity", "waste_basket")),
    ),
    # The countryside's own signature, and the one thing on this list that is
    # visible from a kilometre away.
    PropKind(
        "pylon", (prop_model("power_pole"),),
        height=11.0, share=0.05, priority=7,
        selectors=_tags(("power", "tower"), ("power", "pole")),
    ),
    PropKind(
        "windmill", (prop_model("windmill"),),
        height=20.0, share=0.02, priority=8,
        selectors=_tags(("man_made", "windmill"), ("man_made", "water_tower"),
                        ("man_made", "lighthouse")),
    ),
    PropKind(
        "boulder", (prop_model("boulder"),),
        height=1.4, share=0.04, priority=9, jitter=0.4,
        selectors=_tags(("natural", "rock"), ("natural", "stone")),
    ),
)

_BY_NAME = {kind.name: kind for kind in PROP_KINDS}


def classify(tags: dict[str, str]) -> PropKind | None:
    for kind in PROP_KINDS:
        for key, values in kind.selectors:
            value = tags.get(key)
            if value is not None and value in values:
                return kind
    return None


# ── model heights, measured once ────────────────────────────────────────────

_HEIGHTS: dict[str, float] = {}


def _model_height(game_root: Path, model: str) -> float:
    height = _HEIGHTS.get(model)
    if height is None:
        height = _HEIGHTS[model] = model_height(game_root / model)
    return height


# ── orientation ─────────────────────────────────────────────────────────────

def _road_yaw(x: float, z: float, roads) -> float | None:
    """The bearing of the nearest road segment, or None if none is near.

    A bench at a seeded angle in the middle of a pavement is the single most
    obvious sign of a generator, because a real bench faces something. The
    something is almost always the street, and the street is already in hand.
    """
    best = ROAD_SNAP * ROAD_SNAP
    yaw = None
    for start, end in roads:
        dx, dz = end[0] - start[0], end[1] - start[1]
        length = dx * dx + dz * dz
        if length < 1e-9:
            continue
        t = max(0.0, min(1.0, ((x - start[0]) * dx + (z - start[1]) * dz) / length))
        px, pz = start[0] + t * dx, start[1] + t * dz
        distance = (x - px) ** 2 + (z - pz) ** 2
        if distance < best:
            best = distance
            yaw = math.atan2(dx, dz)
    return yaw


def _quaternion(yaw: float) -> tuple[float, float, float, float]:
    """A rotation about the vertical axis, as the scene format wants it."""
    return (0.0, math.sin(yaw * 0.5), 0.0, math.cos(yaw * 0.5))


# ── placement ───────────────────────────────────────────────────────────────

@dataclass
class PropStats:
    """§4 I5 for rank 10: what was placed, and what the budget refused."""

    placed: int = 0
    dropped: int = 0
    by_kind: dict[str, int] = field(default_factory=dict)

    def as_json(self) -> dict:
        return {"placed": self.placed, "droppedForBudget": self.dropped,
                "byKind": dict(sorted(self.by_kind.items()))}


def _share_out(candidates, budget: int):
    """Give each kind its share, then let the leftovers fall through.

    Two passes and no arithmetic cleverness: the first hands every kind its
    ceiling so that no kind can be starved by a more numerous one, and the
    second spends whatever is left in priority order. A tile whose features fit
    entirely inside the budget keeps all of them and never reaches pass two.
    """
    kept: list = []
    overflow: list = []
    spare = budget
    for kind in PROP_KINDS:
        # A share of zero is *off*, not merely last. Without this the leftover
        # pass below hands a disabled kind whatever the others did not spend,
        # which is how a hundred and sixty-six trees appeared on a tile that had
        # asked for none.
        if kind.share <= 0.0:
            continue
        mine = candidates.get(kind.name, ())
        ceiling = max(1, int(budget * kind.share))
        take = min(len(mine), ceiling, spare)
        kept.extend(mine[:take])
        spare -= take
        overflow.extend(mine[take:])
    taken = min(len(overflow), spare)
    kept.extend(overflow[:taken])
    return kept, len(overflow) - taken


def plan_props(features, ground, profile: RegionProfile, roads, game_root: Path,
               budget: int = TILE_PROP_BUDGET):
    """Turn tagged OSM points into placed, oriented, scaled instances.

    `ground(lon, lat)` puts a coordinate on the terrain, exactly as it does for
    the buildings, so this module never learns what a projection is either.
    """
    candidates: dict[str, list] = {}
    for node in features:
        kind = classify(node.tags)
        if kind is None or kind.share <= 0.0:
            continue
        candidates.setdefault(kind.name, []).append(node)
    # Sorted by id, so which props survive a full tile is the same everywhere
    # and on every run (§4 I3) rather than a function of Overpass's mood.
    for group in candidates.values():
        group.sort(key=lambda node: node.osm_id)

    kept, dropped = _share_out(candidates, budget)
    stats = PropStats(dropped=dropped)
    placed = []
    for node in kept:
        kind = classify(node.tags)
        # A tree's models come from the region, and they are the project's
        # photoscanned ones — never a kit's (CLAUDE.md, rule 1).
        models = kind.models or tuple(tree_model(stem) for stem in profile.tree_models)
        if not models:
            continue
        model = models[seeded(node.osm_id, SALT_MODEL).randrange(len(models))]
        x, y, z = ground(node.lon, node.lat)
        height = kind.height
        if kind.jitter:
            height *= 1.0 + (seeded(node.osm_id, SALT_SCALE).random() - 0.5) * 2.0 * kind.jitter
        scale = height / max(1e-6, _model_height(game_root, model))
        yaw = _road_yaw(x, z, roads) if kind.faces_road else None
        if yaw is None:
            yaw = seeded(node.osm_id, SALT_YAW).random() * math.tau
        placed.append({
            "type": "Node", "name": f"{kind.name} {node.osm_id}", "enabled": True,
            "transform": {"position": [x, y, z], "rotation": list(_quaternion(yaw)),
                          "scale": [scale, scale, scale]},
            "importedFrom": model,
        })
        stats.placed += 1
        stats.by_kind[kind.name] = stats.by_kind.get(kind.name, 0) + 1
    return placed, stats
