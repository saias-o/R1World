"""Link R1World against the existing Saida build without rebuilding/editing it.

The compile definitions/includes come from its generated Ninja target, keeping
the ABI (especially GLM/Jolt/XR) identical to the supplied static library.

The world generator (`native/gen/`) and its two libraries (Clipper2, earcut)
compile into the same binary. Objects are kept between builds and rebuilt
only when their source or a header they may include changed, in parallel.
"""
import argparse
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import shlex
import shutil
import subprocess

GAME = Path(__file__).resolve().parents[1]
ENGINE = GAME.parent / "engine"
BUILD = ENGINE / "build"
OUT = GAME / "generated" / "world-windows"
NATIVE = GAME / "native"


def sources():
    """Every translation unit of the game, and the flags only it needs."""
    # The generator is compiled without contracting a*b+c into one rounding:
    # the same observations must give the same city on every machine (§4 I3).
    gen = ["-ffp-contract=off"]
    units = [(NATIVE / "world.cpp", [])]
    units += [(path, gen) for path in sorted((NATIVE / "gen").glob("*.cpp"))]
    units += [(path, gen) for path in sorted((NATIVE / "third_party" / "clipper2" / "src").glob("*.cpp"))]
    return units


def stale(obj, source, headers):
    if not obj.exists():
        return True
    built = obj.stat().st_mtime
    return source.stat().st_mtime > built or any(h.stat().st_mtime > built for h in headers)


def main(out=OUT):
    ninja = (BUILD / "build.ninja").read_text(encoding="utf-8")
    compile_block = re.search(r"build CMakeFiles/SaidaEngineRuntime.dir/src/runtime/main.cpp.obj:.*?\n(.*?)(?=\n\n)", ninja, re.S).group(1)
    link_block = re.search(r"build bin/SaidaEngineRuntime.exe:.*?\n(.*?)(?=\n\n)", ninja, re.S).group(1)
    def flags(block, key):
        return shlex.split(re.search(r"^  " + key + r" = (.*)$", block, re.M).group(1))
    compiler = Path("C:/msys64/ucrt64/bin/g++.exe")
    env = os.environ.copy()
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    out.mkdir(parents=True, exist_ok=True)
    objects = out / "obj"
    objects.mkdir(exist_ok=True)
    # The traffic add-on is header-only and compiles into this binary, never
    # into the engine library (engine/plugins/traffic/README.md).
    includes = ["-I" + str(ENGINE / "plugins" / "traffic" / "include"), "-I" + str(NATIVE),
                "-I" + str(NATIVE / "third_party" / "clipper2" / "include"),
                "-I" + str(NATIVE / "third_party" / "earcut")]
    headers = list((NATIVE / "gen").glob("*.hpp"))
    common = [*flags(compile_block, "DEFINES"), *flags(compile_block, "INCLUDES"), *includes,
              *flags(compile_block, "FLAGS"), "-O2"]

    def build(unit):
        source, extra = unit
        obj = objects / (source.stem + ".o")
        if stale(obj, source, headers):
            subprocess.run([str(compiler), *common, *extra, "-c", str(source), "-o", str(obj)],
                           cwd=BUILD, env=env, check=True)
        return obj

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        built = list(pool.map(build, sources()))
    subprocess.run([str(compiler), *map(str, built), *flags(link_block, "LINK_FLAGS"),
                    *flags(link_block, "LINK_LIBRARIES"), "-lwinhttp", "-o", str(out / "R1World.exe")],
                   cwd=BUILD, env=env, check=True)
    shutil.copy2(compiler.parent / "glfw3.dll", out / "glfw3.dll")
    print(out / "R1World.exe")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=OUT)
    main(parser.parse_args().output_dir.resolve())
