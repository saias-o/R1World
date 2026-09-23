"""Own the worker and game processes together; no orphan background service."""
import os
from pathlib import Path
import subprocess
import sys
import uuid

GAME = Path(__file__).resolve().parents[1]


def main():
    # Fail before starting either process if authoring dependencies are missing.
    from r1 import street_surfaces
    primary = GAME / "generated" / "world-windows" / "R1World.exe"
    alternate = GAME / "generated" / "world-windows-next" / "R1World.exe"
    if not primary.exists() and not alternate.exists():
        subprocess.run([sys.executable, str(GAME/"tools"/"build_world.py")],check=True)
    exe = max((path for path in (primary, alternate) if path.exists()),
              key=lambda path: path.stat().st_mtime)
    session = GAME / "cache" / "sessions" / uuid.uuid4().hex
    session.mkdir(parents=True)
    env = os.environ.copy()
    env["PYTHONPATH"] = str(GAME/"tools")
    env["PATH"] = "C:/msys64/ucrt64/bin"+os.pathsep+env.get("PATH", "")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    with (session/"worker.log").open("w",encoding="utf-8") as log:
        worker = subprocess.Popen([sys.executable,"-m","r1.world_service","--session",str(session)],
                                  cwd=GAME,env=env,stdout=log,stderr=log,creationflags=flags)
        try:
            with (session/"game.log").open("w",encoding="utf-8") as game_log:
                result=subprocess.run([str(exe),"--project",str(GAME),"--session",str(session),*sys.argv[1:]],
                                      cwd=GAME,env=env,stdout=game_log,stderr=game_log)
            print("Logs:",session)
            return result.returncode
        finally:
            (session/"stop").touch()
            try:worker.wait(timeout=2)
            except subprocess.TimeoutExpired:
                worker.terminate();worker.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
