"""Link R1World against the existing Saida build without rebuilding/editing it.

The compile definitions/includes come from its generated Ninja target, keeping
the ABI (especially GLM/Jolt/XR) identical to the supplied static library.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

GAME = Path(__file__).resolve().parents[1]
ENGINE = GAME.parent / "engine"
BUILD = ENGINE / "build"
OUT = GAME / "generated" / "world-windows"


def main():
    ninja = (BUILD / "build.ninja").read_text(encoding="utf-8")
    compile_block = re.search(r"build CMakeFiles/SaidaEngineRuntime.dir/src/runtime/main.cpp.obj:.*?\n(.*?)(?=\n\n)", ninja, re.S).group(1)
    link_block = re.search(r"build bin/SaidaEngineRuntime.exe:.*?\n(.*?)(?=\n\n)", ninja, re.S).group(1)
    def flags(block, key):
        return shlex.split(re.search(r"^  " + key + r" = (.*)$", block, re.M).group(1))
    compiler = Path("C:/msys64/ucrt64/bin/g++.exe")
    env = os.environ.copy()
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    OUT.mkdir(parents=True, exist_ok=True)
    obj = OUT / "world.o"
    # The traffic add-on is header-only and compiles into this binary, never
    # into the engine library (engine/plugins/traffic/README.md).
    plugins = ["-I" + str(ENGINE / "plugins" / "traffic" / "include")]
    subprocess.run([str(compiler), *flags(compile_block,"DEFINES"), *flags(compile_block,"INCLUDES"),
                    *plugins, *flags(compile_block,"FLAGS"), "-O2", "-c",
                    str(GAME/"native"/"world.cpp"), "-o", str(obj)],cwd=BUILD,env=env,check=True)
    subprocess.run([str(compiler), str(obj), *flags(link_block,"LINK_FLAGS"), *flags(link_block,"LINK_LIBRARIES"),
                    "-o", str(OUT/"R1World.exe")],cwd=BUILD,env=env,check=True)
    shutil.copy2(compiler.parent/"glfw3.dll",OUT/"glfw3.dll")
    print(OUT/"R1World.exe")


if __name__ == "__main__":
    main()
