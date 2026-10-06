"""Fetch the small, redistributable CC0 asset set R1World ships.

Downloads are authoring inputs only.  The game ships the extracted models and
textures, so playing the exported build never depends on a third-party server.
Every payload is pinned by digest to keep regeneration reproducible.
"""

from __future__ import annotations

import hashlib
import json
import struct
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from . import skies
from .normalize import repaint_kit_model
from .vehicle_imports import provenance_records as vehicle_provenance


ROOT = Path(__file__).resolve().parents[3]
GAME_ROOT = ROOT / "game"
SOURCE_ROOT = ROOT / "data" / "source-assets"
MODEL_ROOT = GAME_ROOT / "assets" / "models" / "external" / "kenney_nature_kit"
PROP_ROOT = GAME_ROOT / "assets" / "models" / "external" / "kenney_props"
BOAT_ROOT = GAME_ROOT / "assets" / "models" / "external" / "kenney_boats"
TREE_ROOT = GAME_ROOT / "assets" / "models" / "external" / "trees_lod"
POLYHAVEN_ROOT = GAME_ROOT / "assets" / "models" / "external" / "polyhaven_trees"
TEXTURE_ROOT = GAME_ROOT / "assets" / "textures" / "ground"
LICENSE_ROOT = GAME_ROOT / "assets" / "licenses"
PROVENANCE_PATH = GAME_ROOT / "assets" / "THIRD_PARTY_ASSETS.json"

# The Kenney Nature Kit conifers, named once so the provenance record cannot
# disagree with the disk about which files exist.
# Globbing for them instead makes the record depend on whether the normalised
# copies happened to exist when it was written — an ordering bug waiting for a
# clean checkout to expose it, and one this file already shipped once.
KENNEY_CONIFERS = ("tree_pineTallA", "tree_pineTallB", "tree_pineTallC", "tree_pineRoundA")


def conifer_source(stem: str) -> str:
    """The kit model as downloaded, project-relative."""
    return f"assets/models/external/kenney_nature_kit/{stem}.glb"


@dataclass(frozen=True)
class Download:
    url: str
    target: Path
    algorithm: str
    digest: str


# ── the prop kits ───────────────────────────────────────────────────────────
#
# Rank 10 of §2 — "mobilier urbain régional, forte signature culturelle" —
# needs objects rather than geometry rules: a bench is a bench everywhere, and
# nothing is gained by deriving one. These are the CC0 kits that hold the ones
# OSM actually maps, downloaded whole and mined for a named handful.
#
# **Why whole kits and not single files.** Kenney publishes zips and nothing
# else; there is no per-model URL to pin. The zip is pinned by digest instead
# and lives in `data/source-assets/` beside the Nature Kit, so a clean checkout fetches four archives once and never
# talks to kenney.nl again. What ships in the game is the extracted handful.
PROP_KITS = (
    Download(
        "https://kenney.nl/media/pages/assets/city-kit-roads/74288c9459-1787042796/kenney_city-kit-roads.zip",
        SOURCE_ROOT / "kenney_city-kit-roads.zip", "sha256",
        "22058af3d68173a7cf9bda9f0e243a8cef6bd68168c302ebc76327063849674e",
    ),
    Download(
        "https://kenney.nl/media/pages/assets/graveyard-kit/ba8d4b4517-1760691807/kenney_graveyard-kit_5.0.zip",
        SOURCE_ROOT / "kenney_graveyard-kit_5.0.zip", "sha256",
        "1a93613f2e5675f3310acf49ec9ef13ae7adeb756ac3b205bfb6cc9311a81062",
    ),
    Download(
        "https://kenney.nl/media/pages/assets/fantasy-town-kit/efe948d309-1754222374/kenney_fantasy-town-kit_2.0.zip",
        SOURCE_ROOT / "kenney_fantasy-town-kit_2.0.zip", "sha256",
        "1a7530c09f4d2fa2cdee259876f089334f8b1f27fa86a0c4f54ef86cdd8676ef",
    ),
    # Already on disk; pinned here so a clean checkout gets it
    # from the same place as the other three instead of from nowhere.
    Download(
        "https://kenney.nl/media/pages/assets/nature-kit/2011dc1ad3-1677495570/kenney_nature-kit.zip",
        SOURCE_ROOT / "kenney_nature-kit.zip", "sha256",
        "fa7974a0d342bfe63c38664ba9f8ec1a4aab8ea25f099bdc56870e33588c4d9d",
    ),
    # The boats of the harbours (harbours.py). Rule 1 was read first, both
    # ways: the repository holds no vessel, and Poly Haven's are 17th-century
    # Dutch ships and a pinnace, so a modern fleet widens coverage and
    # displaces nothing -- a boat is a manufactured object, the case the rule
    # leaves to Kenney.
    Download(
        "https://kenney.nl/media/pages/assets/watercraft-kit/a335cfed49-1713519620/kenney_watercraft-pack.zip",
        SOURCE_ROOT / "kenney_watercraft-pack.zip", "sha256",
        "cd1470c1cf441c7f46d0944ae6d0d897242365dc97677c5079b3238965d659f3",
    ),
)


# The models mined out of each kit: `(archive, stem, extracted name)`. Named one
# by one rather than globbed, for the reason the conifer list gives above — a
# provenance record that depends on what happened to be on disk is a record of
# nothing.
PROP_MODELS = (
    # Street furniture. A bus stop is a pole with a sign far more often than it
    # is a shelter, in OSM and on the ground both, so that is what it gets.
    ("kenney_city-kit-roads.zip", "light-square", "street_lamp"),
    ("kenney_city-kit-roads.zip", "light-curved", "street_lamp_curved"),
    ("kenney_city-kit-roads.zip", "road-sign-street", "bus_stop_sign"),
    ("kenney_city-kit-roads.zip", "electricity-pole", "power_pole"),
    ("kenney_city-kit-roads.zip", "dumpster", "waste_bin"),
    ("kenney_graveyard-kit_5.0.zip", "bench", "bench"),
    ("kenney_graveyard-kit_5.0.zip", "lightpost-single", "lamp_ornate"),
    ("kenney_fantasy-town-kit_2.0.zip", "fountain-round", "fountain"),
    ("kenney_fantasy-town-kit_2.0.zip", "lantern", "post_box"),
    ("kenney_fantasy-town-kit_2.0.zip", "windmill", "windmill"),
    # No trees. The project has photoscanned ones and they are better; a kit
    # tree entering here would displace them, which is rule 1 of CLAUDE.md and
    # was broken once already. Vegetation comes from TREES below.
    ("kenney_nature-kit.zip", "rock_smallA", "boulder"),
)


# Road vehicles are imported from pinned licensed sources and shared across the world.



# ── the photoreal trees ─────────────────────────────────────────────────────
#
# Poly Haven, CC0, photoscanned, and the best asset in the repository. They are
# also enormous — fir_sapling is 515 299 vertices, 49% of Saida's whole geometry
# arena, and two species would leave no room for a city. So they enter
# decimated (see `decimate.py`) rather than exchanged for something cheaper.
#
# `stem` is the file on disk; `name` is what a scene references.
TREES = (
    (MODEL_ROOT, "fir_sapling_1k", "fir_sapling"),
    (MODEL_ROOT, "pine_sapling_small_1k", "pine_sapling"),
    (MODEL_ROOT, "quiver_tree_01_1k", "quiver_tree"),
    (MODEL_ROOT, "quiver_tree_02_1k", "quiver_tree_slim"),
    # The broadleaf. Conifers and a desert aloe were what happened to be on
    # disk, and a plane-lined Paris boulevard is neither.
    (POLYHAVEN_ROOT, "island_tree_01_1k", "broadleaf"),
)

# Vertices each decimated species may keep. Four species is 60 k, which sits
# inside the arena beside the densest neighbourhood measured (841 035) and the
# prop library (9 390) with room for the player, the UI and the sky.
TREE_VERTEX_BUDGET = 12_000


def tree_model(name: str) -> str:
    """The project-relative path a scene references a decimated tree by."""
    return f"assets/models/external/trees_lod/{name}.glb"


def decimate_trees() -> list[dict]:
    """Bring every photoreal tree into the arena's budget, once.

    Idempotent and cheap — about a second a species — so it runs on every
    generation rather than behind a flag nobody remembers to pass.
    """
    from .decimate import decimate_gltf
    TREE_ROOT.mkdir(parents=True, exist_ok=True)
    report = []
    for folder, stem, name in TREES:
        source = folder / f"{stem}.gltf"
        if not source.exists():
            raise RuntimeError(f"{source} is missing; run ensure_external_assets()")
        target = TREE_ROOT / f"{name}.glb"
        report.append(decimate_gltf(source, target, TREE_VERTEX_BUDGET))
    return report


def prop_model(name: str) -> str:
    """The project-relative path a scene references this prop by."""
    return f"assets/models/external/kenney_props/{name}.glb"


# The fleet: every hull `harbours.BOATS` can moor, plus the containers of a
# port's yard. The galleons of the kit (`ship-large`, `ship-small`) are left
# in the archive: a 17th-century ship in a modern harbour is a costume.
BOAT_MODELS = tuple(
    ("kenney_watercraft-pack.zip", stem, stem.replace("-", "_"))
    for stem in (
        "boat-speed-a", "boat-speed-c", "boat-speed-e", "boat-speed-g", "boat-speed-j",
        "boat-sail-a", "boat-sail-b", "boat-fishing-small", "boat-row-large", "boat-row-small",
        "boat-tug-a", "ship-cargo-a", "ship-cargo-b", "ship-cargo-c", "ship-ocean-liner",
        "boat-house-a", "boat-house-b", "boat-house-c", "boat-house-d",
        "cargo-container-a", "cargo-container-b", "cargo-container-c",
    )
)


def boat_model(name: str) -> str:
    """The project-relative path a scene references this boat by."""
    return f"assets/models/external/kenney_boats/{name}.glb"


def extract_boats() -> tuple[str, ...]:
    """The fleet, mined out of the watercraft kit through the same normaliser."""
    return _mine(BOAT_MODELS, BOAT_ROOT, boat_model)


def vehicle_model(name: str) -> str:
    """The project-relative path a scene references this vehicle by."""
    from .vehicle_fleet import model
    return model(name)


# The prop palette, in albedo (§4, and the same argument as `ground.py`).
# Kenney paints foliage (0.16, 0.79, 0.67) — a turquoise four times as bright
# as a leaf — and bark (0.89, 0.51, 0.34), a salmon brighter than snow-free
# anything. Both saturate on sight under this world's Sun.
PROP_PALETTE: dict[str, tuple[float, float, float]] = {
    "leafsGreen": (0.088, 0.150, 0.075),
    "leafsDark": (0.062, 0.105, 0.058),
    "grass": (0.110, 0.165, 0.085),
    "woodBark": (0.150, 0.115, 0.085),
    "woodBarkDark": (0.105, 0.082, 0.062),
    "dirt": (0.190, 0.160, 0.125),
    "Water": (0.021, 0.034, 0.047),
}

# Materials that keep their image and have it dimmed instead. One factor for
# the lot: the atlas is one atlas, and a per-model factor would be a look tuned
# model by model, which is how a palette stops being a palette.
PROP_TEXTURE_TINT: dict[str, tuple[float, float, float]] = {
    "colormap": (0.50, 0.50, 0.50),
}


def _embed_images(payload: bytes, resolve) -> bytes:
    """Pull every externally referenced image into the GLB's own binary chunk.

    Kenney's newer kits do not embed their texture: every model in a kit points
    at one shared `Textures/colormap.png`, a small palette atlas the UVs sample
    a flat colour from. Extracting the `.glb` alone therefore extracts a model
    with a dangling URI, and the engine draws the missing-texture magenta —
    which is exactly what the first render of this showed, a street of bright
    purple lamp posts.

    Copying the atlas next to the models would work and is not done, because it
    makes a prop a *pair* of files that a later move can separate. At eleven
    kilobytes an atlas, embedding costs about a hundred kilobytes across the
    whole set and makes each prop a single file that cannot be half-installed.
    """
    json_length, _ = struct.unpack_from("<II", payload, 12)
    document = json.loads(payload[20:20 + json_length].decode("utf-8"))
    binary_header = 20 + json_length
    binary_length, binary_type = struct.unpack_from("<II", payload, binary_header)
    binary = bytearray(payload[binary_header + 8:binary_header + 8 + binary_length])

    images = document.get("images", [])
    if not any(image.get("uri") for image in images):
        return payload
    views = document.setdefault("bufferViews", [])
    for image in images:
        uri = image.get("uri")
        if not uri:
            continue
        blob = resolve(uri)
        if blob is None:
            raise RuntimeError(f"the kit has no texture named {uri}")
        binary += b"\0" * ((-len(binary)) % 4)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(blob)})
        binary += blob
        image.pop("uri")
        image["bufferView"] = len(views) - 1
        image["mimeType"] = "image/png" if uri.lower().endswith(".png") else "image/jpeg"
    binary += b"\0" * ((-len(binary)) % 4)
    if document.get("buffers"):
        document["buffers"][0]["byteLength"] = len(binary)
        document["buffers"][0].pop("uri", None)
    else:
        document["buffers"] = [{"byteLength": len(binary)}]

    encoded = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
    encoded += b" " * ((-len(encoded)) % 4)
    total = 12 + 8 + len(encoded) + 8 + len(binary)
    return (
        struct.pack("<III", 0x46546C67, 2, total)
        + struct.pack("<II", len(encoded), 0x4E4F534A)
        + encoded
        + struct.pack("<II", len(binary), binary_type)
        + bytes(binary)
    )


def _mine(models, root: Path, reference) -> tuple[str, ...]:
    """Mine named models out of the kit archives into `root`, once.

    The archives are authoring inputs and are never read at run time; what the
    game opens is the extracted GLB. Extraction is idempotent and cheap, so it
    runs on every generation rather than behind a flag nobody would remember to
    pass after adding a model.

    Props and the car share this because they must share the *normaliser*: the
    car is a Kenney kit like the rest, it samples the same `colormap.png` atlas
    authored as paint, and a second extraction path would be a second place for
    a kit's own colours to walk in (§4). One path, one palette check.
    """
    root.mkdir(parents=True, exist_ok=True)
    written = []
    archives: dict[str, zipfile.ZipFile] = {}
    try:
        for archive_name, stem, target_name in models:
            archive = archives.get(archive_name)
            if archive is None:
                path = SOURCE_ROOT / archive_name
                if not path.exists():
                    raise RuntimeError(
                        f"{path} is missing; the kits are authoring inputs and "
                        "are fetched by ensure_external_assets()")
                archive = archives[archive_name] = zipfile.ZipFile(path)
            members = [n for n in archive.namelist()
                       if n.endswith(".glb") and n.split("/")[-1][:-4] == stem]
            if not members:
                raise RuntimeError(f"{archive_name} has no model named {stem}")
            member = members[0]
            folder = member.rsplit("/", 1)[0] if "/" in member else ""

            def resolve(uri: str, _folder=folder, _archive=archive) -> bytes | None:
                candidate = f"{_folder}/{uri}" if _folder else uri
                try:
                    return _archive.read(candidate)
                except KeyError:
                    tail = uri.rsplit("/", 1)[-1]
                    for name in _archive.namelist():
                        if name.endswith("/" + tail) and name.startswith(_folder):
                            return _archive.read(name)
                return None

            payload = repaint_kit_model(
                _embed_images(_sanitize_kenney_glb(archive.read(member)), resolve),
                PROP_PALETTE, PROP_TEXTURE_TINT, name=f"{target_name}.glb")
            target = root / f"{target_name}.glb"
            # Only touch the file when it would change: the scene generator
            # hashes what it ships, and a rewritten-but-identical GLB would
            # churn the manifest on every run for nothing.
            if not target.exists() or target.read_bytes() != payload:
                target.write_bytes(payload)
            written.append(reference(target_name))
    finally:
        for archive in archives.values():
            archive.close()
    return tuple(written)


def extract_props() -> tuple[str, ...]:
    """Street furniture and vegetation, mined out of the four prop kits."""
    return _mine(PROP_MODELS, PROP_ROOT, prop_model)


def extract_vehicles() -> tuple[str, ...]:
    """Bake pinned licensed sources with Blender; no runtime downloads."""
    from . import vehicle_fleet
    vehicle_fleet.main()
    return tuple(vehicle_fleet.model(n,far) for n in vehicle_fleet.SPECS for far in (False,True))


def extract_aircraft() -> tuple[str, ...]:
    """Regenerate the original aircraft: the airports' fleet and the bases' helicopters."""
    from . import aircraft_fleet
    aircraft_fleet.main()
    return tuple(aircraft_fleet.model(n, far) for n in aircraft_fleet.FLEET for far in (False, True))



TREE_MODELS = (
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/pine_sapling_small/pine_sapling_small_1k.gltf",
        MODEL_ROOT / "pine_sapling_small_1k.gltf",
        "md5",
        "237a7672645631550ae571743aa9f55b",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/pine_sapling_small/pine_sapling_small.bin",
        MODEL_ROOT / "pine_sapling_small.bin",
        "md5",
        "76dd1802d8de128c812f2e124a258b35",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/fir_sapling/fir_sapling_1k.gltf",
        MODEL_ROOT / "fir_sapling_1k.gltf",
        "md5",
        "7b1a5ceae7be69954510b5a5c719b4fb",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/fir_sapling/fir_sapling.bin",
        MODEL_ROOT / "fir_sapling.bin",
        "md5",
        "b329143a90d95201891afc52daeb9698",
    ),
)

TREE_TEXTURES = (
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/pine_sapling_small/pine_sapling_small_bark_diff_1k.jpg",
        MODEL_ROOT / "textures" / "pine_sapling_small_bark_diff_1k.jpg",
        "md5",
        "7d3a558ed614c7e75594c7be3bf80311",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/pine_sapling_small/pine_sapling_small_bark_nor_gl_1k.jpg",
        MODEL_ROOT / "textures" / "pine_sapling_small_bark_nor_gl_1k.jpg",
        "md5",
        "bb6e3a6eea777d992cef48fabfdc538c",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/pine_sapling_small/pine_sapling_small_bark_arm_1k.jpg",
        MODEL_ROOT / "textures" / "pine_sapling_small_bark_arm_1k.jpg",
        "md5",
        "21d1abd15e40951825a4f3da110b20ea",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/pine_sapling_small/pine_sapling_small_twig_diff_1k.jpg",
        MODEL_ROOT / "textures" / "pine_sapling_small_twig_diff_1k.jpg",
        "md5",
        "dacc918bb421968e9ccc1956d78a2dbe",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/pine_sapling_small/pine_sapling_small_twig_nor_gl_1k.jpg",
        MODEL_ROOT / "textures" / "pine_sapling_small_twig_nor_gl_1k.jpg",
        "md5",
        "eebc5a81f8f6231d8e18ec4ad7292798",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/pine_sapling_small/pine_sapling_small_twig_arm_1k.jpg",
        MODEL_ROOT / "textures" / "pine_sapling_small_twig_arm_1k.jpg",
        "md5",
        "b5062030fc42ec7bd12eb3f052054104",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/fir_sapling/fir_sapling_branches_diff_1k.jpg",
        MODEL_ROOT / "textures" / "fir_sapling_branches_diff_1k.jpg",
        "md5",
        "84f1d40b80a015d65c0ded042a10765b",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/fir_sapling/fir_sapling_branches_nor_gl_1k.jpg",
        MODEL_ROOT / "textures" / "fir_sapling_branches_nor_gl_1k.jpg",
        "md5",
        "db70c4595cf207cc04a02b49c195b78d",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/fir_sapling/fir_sapling_branches_arm_1k.jpg",
        MODEL_ROOT / "textures" / "fir_sapling_branches_arm_1k.jpg",
        "md5",
        "29a9ff22b8f7f224069b1254db171c05",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/fir_sapling/fir_sapling_twigs_diff_1k.jpg",
        MODEL_ROOT / "textures" / "fir_sapling_twigs_diff_1k.jpg",
        "md5",
        "cd218269e0b34bd1bdb48a8daf21be67",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/fir_sapling/fir_sapling_twigs_nor_gl_1k.jpg",
        MODEL_ROOT / "textures" / "fir_sapling_twigs_nor_gl_1k.jpg",
        "md5",
        "764771b717c54b86fa7d3eadc4a1ada5",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/fir_sapling/fir_sapling_twigs_arm_1k.jpg",
        MODEL_ROOT / "textures" / "fir_sapling_twigs_arm_1k.jpg",
        "md5",
        "cf97b5de863ebad9a48cb187ef3c3e74",
    ),
)


GROUND_TEXTURES = (
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/forrest_ground_01/forrest_ground_01_diff_1k.jpg",
        TEXTURE_ROOT / "forrest_ground_01_diff_1k.jpg",
        "md5",
        "236e7d928f5e357a194fd92de189cbe4",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/forrest_ground_01/forrest_ground_01_nor_gl_1k.jpg",
        TEXTURE_ROOT / "forrest_ground_01_nor_gl_1k.jpg",
        "md5",
        "ba4265df25aea293913d004b69ef9ab0",
    ),
    Download(
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/forrest_ground_01/forrest_ground_01_rough_1k.jpg",
        TEXTURE_ROOT / "forrest_ground_01_rough_1k.jpg",
        "md5",
        "72dacf5b829cafc025f08ca64e92e5eb",
    ),
)


def _digest(path: Path, algorithm: str) -> str:
    hasher = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def _fetch(download: Download) -> None:
    if download.target.exists() and _digest(download.target, download.algorithm) == download.digest:
        return
    download.target.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(
        download.url,
        headers={"User-Agent": "R1World-generator/1.0"},
    )
    temporary = download.target.with_suffix(download.target.suffix + ".download")
    with urllib.request.urlopen(request, timeout=120.0) as response, temporary.open("wb") as output:
        while block := response.read(1024 * 1024):
            output.write(block)
    actual = _digest(temporary, download.algorithm)
    if actual != download.digest:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(
            f"digest mismatch for {download.url}: expected {download.digest}, got {actual}"
        )
    temporary.replace(download.target)


def _sanitize_kenney_glb(payload: bytes) -> bytes:
    """Repair the old UniGLTF scene roots and normalize non-metallic foliage."""
    magic, version, _ = struct.unpack_from("<III", payload, 0)
    if magic != 0x46546C67 or version != 2:
        raise RuntimeError("Kenney Nature Kit model is not a GLB 2.0 file")
    json_length, json_type = struct.unpack_from("<II", payload, 12)
    if json_type != 0x4E4F534A:
        raise RuntimeError("Kenney Nature Kit GLB has no JSON chunk")
    document = json.loads(payload[20:20 + json_length].decode("utf-8"))
    binary_header = 20 + json_length
    binary_length, binary_type = struct.unpack_from("<II", payload, binary_header)
    binary = payload[binary_header + 8:binary_header + 8 + binary_length]

    child_nodes = {
        child
        for node in document.get("nodes", [])
        for child in node.get("children", [])
    }
    roots = [index for index in range(len(document.get("nodes", []))) if index not in child_nodes]
    for scene in document.get("scenes", []):
        scene["nodes"] = roots
    for material in document.get("materials", []):
        material.setdefault("pbrMetallicRoughness", {})["metallicFactor"] = 0.0

    encoded = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
    encoded += b" " * ((-len(encoded)) % 4)
    binary += b"\0" * ((-len(binary)) % 4)
    total = 12 + 8 + len(encoded) + 8 + len(binary)
    return (
        struct.pack("<III", magic, version, total)
        + struct.pack("<II", len(encoded), json_type)
        + encoded
        + struct.pack("<II", len(binary), binary_type)
        + binary
    )


def surfaces_provenance() -> list[dict]:
    """One record per scanned material of `r1/surfaces.py`, from its table."""
    from . import surfaces
    try:
        families = surfaces.load_table()["families"]
    except FileNotFoundError:
        return []
    by_asset: dict[tuple[str, str], list[str]] = {}
    for entry in families.values():
        files = by_asset.setdefault((entry["source"], entry["asset"]), [])
        files += [entry["albedo"], entry["normal"], entry["mr"]]
    records = []
    for (origin, asset), files in sorted(by_asset.items()):
        records.append({
            "name": f"{asset} (surface material)",
            "author": "Poly Haven" if origin == "polyhaven" else "ambientCG (Lennart Demes)",
            "source": (f"https://polyhaven.com/a/{asset}" if origin == "polyhaven"
                       else f"https://ambientcg.com/view?id={asset}"),
            "license": "CC0 1.0",
            "modified": "Colour normalised to the palette albedo, resized; walls baked "
                        "into bay sheets with a window (r1/surfaces.py)",
            "files": sorted(set(files)),
        })
    return records


def ensure_external_assets() -> dict[str, object]:
    """Download, verify and extract the CC0 models/textures used by R1World."""
    for kit in PROP_KITS:
        _fetch(kit)
    props = extract_props()
    vehicles = extract_vehicles()
    aircraft = extract_aircraft()
    boats = extract_boats()
    for model in TREE_MODELS:
        _fetch(model)
    for texture in TREE_TEXTURES:
        _fetch(texture)
    for texture in GROUND_TEXTURES:
        _fetch(texture)

    MODEL_ROOT.mkdir(parents=True, exist_ok=True)
    LICENSE_ROOT.mkdir(parents=True, exist_ok=True)

    (LICENSE_ROOT / "Kenney-CC0.txt").write_text(
        """Assets by Kenney (www.kenney.nl).
City Kit (Roads), Graveyard Kit, Fantasy Town Kit, Nature Kit, Car Kit and
Watercraft Kit.
Dedicated to the public domain under CC0 1.0 Universal.
http://creativecommons.org/publicdomain/zero/1.0/
Crediting Kenney is appreciated and is not required.
""",
        encoding="utf-8",
    )
    (LICENSE_ROOT / "ambientCG-CC0.txt").write_text(
        "Materials by ambientCG (Lennart Demes).\n"
        "Dedicated to the public domain under CC0 1.0 Universal.\n"
        "Source: https://ambientcg.com/\n"
        "License: https://docs.ambientcg.com/license/\n",
        encoding="utf-8",
    )
    poly_license = LICENSE_ROOT / "Poly-Haven-CC0.txt"
    poly_license.write_text(
        "Assets by Poly Haven.\n"
        "Dedicated to the public domain under CC0 1.0 Universal.\n"
        "Source: https://polyhaven.com/\n"
        "License: https://polyhaven.com/license\n",
        encoding="utf-8",
    )

    tree_files = [m.target for m in TREE_MODELS] + [t.target for t in TREE_TEXTURES]
    provenance = {
        "schema": 1,
        "assets": [
            {
                "name": "Poly Haven Trees (Pine Sapling & Fir Sapling)",
                "source": "https://polyhaven.com/models",
                "license": "CC0 1.0",
                "files": [path.relative_to(GAME_ROOT).as_posix() for path in tree_files],
            },
            {
                # On disk and not planted: kept because they are the low-cost
                # conifers the vegetation budget will want back once §4 can
                # decimate the photoreal ones.
                "name": "Kenney Nature Kit — conifers",
                "usedInScene": False,
                "author": "Kenney",
                "source": "https://kenney.nl/assets/nature-kit",
                "license": "CC0 1.0",
                "files": [conifer_source(stem) for stem in KENNEY_CONIFERS],
            },
            {
                # Rank 8 and rank 10 for the whole planet, in 436 kB. Extracted
                # from four CC0 kits; the archives are authoring inputs and are
                # not shipped.
                "name": "Kenney props — street furniture and vegetation",
                "author": "Kenney",
                "source": "https://kenney.nl/assets/city-kit-roads, "
                          "https://kenney.nl/assets/graveyard-kit, "
                          "https://kenney.nl/assets/fantasy-town-kit, "
                          "https://kenney.nl/assets/nature-kit",
                "license": "CC0 1.0",
                "extractedFrom": [kit.target.name for kit in PROP_KITS],
                "files": list(props),
            },
            *vehicle_provenance(),
            {
                # Original aircraft for airports and military bases, both levels.
                "name": "R1World original aircraft",
                "author": "R1World",
                "source": "game/tools/r1/aircraft_fleet.py",
                "license": "CC0 1.0",
                "generatedBy": "game/tools/r1/aircraft_fleet.py",
                "files": list(aircraft),
            },
            {
                # The harbours' fleet and the containers of their yards.
                "name": "Kenney Watercraft Kit — boats, ships and containers",
                "author": "Kenney",
                "source": "https://kenney.nl/assets/watercraft-kit",
                "license": "CC0 1.0",
                "extractedFrom": ["kenney_watercraft-pack.zip"],
                "files": list(boats),
            },
            {
                "name": "Forest Ground 01",
                "author": "Poly Haven",
                "source": "https://polyhaven.com/",
                "license": "CC0 1.0",
                "files": [
                    texture.target.relative_to(GAME_ROOT).as_posix()
                    for texture in GROUND_TEXTURES
                ],
            },
            {
                # The sky at every hour. Downloaded and normalised by
                # `r1/skies.py`, which removes each photograph's Sun; the
                # downloads are authoring inputs in data/source-assets/skies.
                "name": "Qwantani sky series (pure sky)",
                "author": "Poly Haven (photography Greg Zaal, processing Jarod Guest)",
                "source": "https://polyhaven.com/hdris",
                "license": "CC0 1.0",
                "modified": "Sun disc, aureole and lens streaks removed (r1/skies.py)",
                "files": [source.texture for source in skies.SOURCES]
                         + ["assets/skies/skies.json"],
            },
        ] + surfaces_provenance(),
    }
    PROVENANCE_PATH.write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    return {
        "trees": tuple(m.target for m in TREE_MODELS if m.target.suffix == ".gltf"),
        "groundDiffuse": GROUND_TEXTURES[0].target,
        "groundNormal": GROUND_TEXTURES[1].target,
        "groundRoughness": GROUND_TEXTURES[2].target,
        "provenance": PROVENANCE_PATH,
    }


if __name__ == "__main__":
    ensure_external_assets()
