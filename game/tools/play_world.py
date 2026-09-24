"""Run the game from the development tree, logging to a session folder.

The same thing `Play.ps1` does, for the test drivers: the world is generated
inside the game, so there is no worker to start beside it.
"""
import os
from pathlib import Path
import subprocess
import sys
import uuid

GAME = Path(__file__).resolve().parents[1]


def main():
    primary = GAME / "generated" / "world-windows" / "R1World.exe"
    alternate = GAME / "generated" / "world-windows-next" / "R1World.exe"
    if not primary.exists() and not alternate.exists():
        subprocess.run([sys.executable, str(GAME / "tools" / "build_world.py")], check=True)
    exe = max((path for path in (primary, alternate) if path.exists()), key=lambda path: path.stat().st_mtime)
    session = GAME / "cache" / "sessions" / uuid.uuid4().hex
    session.mkdir(parents=True)
    env = os.environ.copy()
    env["PATH"] = "C:/msys64/ucrt64/bin" + os.pathsep + env.get("PATH", "")
    with (session / "game.log").open("w", encoding="utf-8") as log:
        result = subprocess.run([str(exe), "--project", str(GAME), *sys.argv[1:]],
                                cwd=GAME, env=env, stdout=log, stderr=log)
    print("Logs:", session)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
