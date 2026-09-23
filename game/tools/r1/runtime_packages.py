"""Restore the shipped, hash-pinned wheels before importing native packages.

Runs in the player's process, offline, using only Python's standard library.
Never trusts a partial package installation or an import of its top-level name.
"""
import hashlib
import importlib
import os
from pathlib import Path, PurePosixPath
import sys
import uuid
import zipfile
import zlib

WHEELS = (
    ("numpy-2.5.3-cp312-cp312-win_amd64.whl",
     "0a59a421a32580a009e8a1751345bf829631b990dc1794b80514ab722b435def"),
    ("shapely-2.1.2-cp312-cp312-win_amd64.whl",
     "743044b4cfb34f9a67205cee9279feaf60ba7d02e69febc2afc609047cb49179"),
)
_ready = None


def repair_wheel(archive, destination, digest):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise RuntimeError("Archive du jeu endommagee : " + str(archive))
    repaired = 0
    with zipfile.ZipFile(archive) as wheel:
        for entry in wheel.infolist():
            relative = PurePosixPath(entry.filename)
            if relative.is_absolute() or ".." in relative.parts or "\\" in entry.filename or ":" in entry.filename:
                raise ValueError("Unsafe wheel path")
            if entry.is_dir():
                continue
            path = destination.joinpath(*relative.parts)
            try:
                valid = path.stat().st_size == entry.file_size and \
                    zlib.crc32(path.read_bytes()) == entry.CRC
            except FileNotFoundError:
                valid = False
            if valid:
                continue
            path.parent.mkdir(parents=True,exist_ok=True)
            temporary = path.with_name(path.name+"."+uuid.uuid4().hex+".tmp")
            try:
                temporary.write_bytes(wheel.read(entry))
                os.replace(temporary,path)
            finally:
                temporary.unlink(missing_ok=True)
            repaired += 1
    return repaired


def ensure_dependencies():
    global _ready
    if _ready is not None:
        return _ready
    generated = Path(__file__).resolve().parents[2]/"generated"
    destination = generated/"runtime-packages-v1"
    repaired = sum(repair_wheel(generated/"runtime-wheels"/name,destination,digest)
                   for name,digest in WHEELS)
    sys.path.insert(0,str(destination))
    importlib.invalidate_caches()
    _ready = destination
    if repaired:
        print("[R1World] Dependances locales preparees :",repaired,"fichiers.",flush=True)
    return destination
