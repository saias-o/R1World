"""The asset normaliser — plan §4.

> Incohérence visuelle des assets CC0 | Moyenne | Normalisateur obligatoire
> (§4). Un asset non conforme n'entre pas.

A world built from free assets is built from assets authored by different people
for different projects under different lighting, and the plan names their
inconsistency as a standing risk with a standing answer: nothing enters the game
in the state it was downloaded in. This module is the first piece of that
answer, and it does one thing — it rewrites a model's material colours into the
region's palette (§9) so a kit's own styling does not overrule the Atlas.

The immediate case is the Kenney Nature Kit conifers. They are good geometry at
a few hundred vertices, which is exactly what §12.2 wants standing in a village,
and they are painted in the kit's own palette: teal foliage and salmon bark.
Dropped unmodified into a valley lit by computed alpine sunlight over a
photographed forest floor, the teal is the first thing the eye lands on and the
last thing it forgives.

**Why by name and not by heuristic.** The kit names its materials `leafsDark`
and `woodBarkDark`, so the mapping is exact. A heuristic — "recolour whatever
looks greenest" — would keep working silently when it started being wrong, and
this module's whole purpose is that a non-conforming asset does not enter. An
absent material is therefore an error, not a skipped step: if the kit renames
`leafsDark` tomorrow, the build stops rather than shipping teal trees.

**Why a copy.** The downloaded model is an input and stays byte-identical to
what was fetched and checksummed; the normalised version is an output of the
generator like every other GLB it writes. Editing the source in place would
break the provenance record and make the pipeline non-idempotent.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

Colour = tuple[float, float, float]

_GLB_MAGIC = 0x46546C67
_CHUNK_JSON = 0x4E4F534A
_CHUNK_BIN = 0x004E4942


def _read_glb(payload: bytes) -> tuple[dict, bytes]:
    magic, version, _ = struct.unpack_from("<III", payload, 0)
    if magic != _GLB_MAGIC or version != 2:
        raise RuntimeError("not a GLB 2.0 file")
    json_length, json_type = struct.unpack_from("<II", payload, 12)
    if json_type != _CHUNK_JSON:
        raise RuntimeError("GLB has no JSON chunk")
    document = json.loads(payload[20:20 + json_length].decode("utf-8"))

    binary = b""
    cursor = 20 + json_length
    while cursor + 8 <= len(payload):
        chunk_length, chunk_type = struct.unpack_from("<II", payload, cursor)
        if chunk_type == _CHUNK_BIN:
            binary = payload[cursor + 8:cursor + 8 + chunk_length]
            break
        cursor += 8 + chunk_length
    return document, binary


def _write_glb(document: dict, binary: bytes) -> bytes:
    # Sorted keys and fixed separators: the same input must produce the same
    # bytes on every machine and in every run (§3 I3), and a GLB whose hash
    # moves between runs makes the build manifest meaningless.
    encoded = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
    encoded += b" " * ((-len(encoded)) % 4)
    binary += b"\0" * ((-len(binary)) % 4)
    total = 12 + 8 + len(encoded) + 8 + len(binary)
    out = struct.pack("<III", _GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(encoded), _CHUNK_JSON) + encoded
    if binary:
        out += struct.pack("<II", len(binary), _CHUNK_BIN) + binary
    return out


def repaint_kit_model(
    payload: bytes,
    palette: dict[str, Colour],
    textured: dict[str, Colour],
    name: str = "model",
) -> bytes:
    """Repaint one kit model in place, and refuse a material nobody named.

    The props arrive from four kits and share a small vocabulary of material
    names — `leafsGreen`, `woodBark`, `colormap` — so a table keyed by name
    covers twenty models without twenty rows. What it must not do is silently
    pass over a name it does not know, because that is a prop entering the game
    in the kit's own colours, which is the thing §4 exists to prevent. Every
    material must therefore be in one of the two tables, and the return value
    names what was repainted so the caller can say so.

    `palette` replaces a flat colour outright. `textured` multiplies a material
    that carries an image: Kenney's shared `colormap.png` is a palette atlas
    authored as paint, around 0.8 where real painted metal is 0.2 and a road
    sign is 0.6, and a factor is the only way to bring an atlas into the range
    without repainting the atlas.

    The reason both are needed is the same reason the ground table carries
    albedos: at this world's light level a horizontal or sunlit surface above
    roughly 0.35 saturates, and a saturated prop is a white blob whatever it
    was modelled as. Kenney's `leafsGreen` is (0.16, 0.79, 0.67) — a turquoise
    at four times the albedo of a real leaf — and a street of those reads as
    cyan lollipops, which is precisely how this was found.

    It works on bytes rather than on a file because extraction, embedding and
    repainting are three steps of one transformation, and writing the
    intermediate results out would make the extracted model differ from itself
    between two runs that produced the same thing.
    """
    document, binary = _read_glb(payload)
    materials = document.get("materials", [])
    names = [material.get("name") for material in materials]
    unknown = sorted(n for n in names if n not in palette and n not in textured)
    if unknown:
        raise RuntimeError(
            f"{name} declares material(s) {', '.join(unknown)}, which the "
            f"prop palette does not name. The asset would enter in the kit's "
            f"own colours (plan §4); add it to the table or drop the model."
        )
    for material in materials:
        colour = palette.get(material.get("name")) or textured[material.get("name")]
        pbr = material.setdefault("pbrMetallicRoughness", {})
        alpha = 1.0
        existing = pbr.get("baseColorFactor")
        if isinstance(existing, list) and len(existing) == 4:
            alpha = float(existing[3])
        pbr["baseColorFactor"] = [colour[0], colour[1], colour[2], alpha]
        # Nothing in a village is chrome, and kits routinely leave the metallic
        # factor at its default of 1.
        pbr["metallicFactor"] = 0.0
    return _write_glb(document, binary)


def recolour_model(
    source: Path,
    target: Path,
    palette: dict[str, Colour],
    roughness: dict[str, float] | None = None,
) -> None:
    """Write `source` to `target` with its named materials repainted.

    `palette` maps a material name in the model to a linear RGB colour. Every
    name in it must exist in the model; a name that does not is a normaliser
    that has silently stopped normalising, and it raises.

    Alpha is preserved — a kit that uses it for alpha-tested foliage still
    works — and so is every other property of the material. This repaints; it
    does not re-author.
    """
    document, binary = _read_glb(source.read_bytes())
    materials = document.get("materials", [])
    by_name = {material.get("name"): material for material in materials}

    missing = sorted(name for name in palette if name not in by_name)
    if missing:
        raise RuntimeError(
            f"{source.name} has no material named {', '.join(missing)} — "
            f"it declares {sorted(n for n in by_name if n)}. The normaliser is "
            f"mapping names that no longer exist, so the asset would enter "
            f"unchanged (plan §4)."
        )

    for name, colour in palette.items():
        pbr = by_name[name].setdefault("pbrMetallicRoughness", {})
        alpha = 1.0
        existing = pbr.get("baseColorFactor")
        if isinstance(existing, list) and len(existing) == 4:
            alpha = float(existing[3])
        pbr["baseColorFactor"] = [colour[0], colour[1], colour[2], alpha]
        # Foliage and bark are dielectric. Kits routinely ship a metallic
        # factor of 1 by omission, which turns a tree into chrome under IBL.
        pbr["metallicFactor"] = 0.0
        if roughness and name in roughness:
            pbr["roughnessFactor"] = roughness[name]

    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(_write_glb(document, binary))
