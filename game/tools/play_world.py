"""Run the game from the development tree, logging to a session folder.

The same thing `Play.ps1` does, for the test drivers: the world is generated
inside the game, so there is no worker to start beside it. The executable is
brought up to date by `Play.ps1 -BuildOnly` first -- the one build there is --
so a test never runs a binary older than the sources it is testing.
"""
import os
from pathlib import Path
import subprocess
import sys
import uuid

GAME = Path(__file__).resolve().parents[1]


def main():
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", str(GAME / "Play.ps1"), "-BuildOnly"], check=True)
    exe = GAME / "generated" / "world-windows" / "R1World.exe"
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
