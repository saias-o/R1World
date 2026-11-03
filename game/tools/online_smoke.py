"""Two players at one spot, on this machine, with no server: online play end to end.

Builds once (`Play.ps1 -BuildOnly`), then starts two smoke instances a few
metres apart: a target that stands, and a shooter that fires at the body it
draws of the other. They find each other through the LAN beacons and the
Mainline DHT (plugins/p2p), each draws the other from its snapshots, and the
target's machine judges the shots. Both must log PASS.

    python tools/online_smoke.py [--versions] [lon lat]

--versions plays the second instance as another build (another protocol in
its hello): each must link the other, say "on another version" on its
panel, and never draw him.

Visit the spot once first (play_world.py --smoke --spawn lon lat): two
instances cooking the same tiles at once would only measure the cook.
"""
import math
import os
from pathlib import Path
import subprocess
import sys
import uuid

GAME = Path(__file__).resolve().parents[1]


def main():
    args = [a for a in sys.argv[1:] if a != "--versions"]
    versions = len(args) < len(sys.argv) - 1
    lon, lat = (float(args[0]), float(args[1])) if len(args) >= 2 else (2.3522, 48.8566)
    roles = (("versions", 0.0, None), ("versions", 6.0, "r1world/online-smoke-other")) if versions else         (("target", 0.0, None), ("shooter", 6.0, None))
    env = {key.upper(): value for key, value in os.environ.items()}
    env["PATH"] = "C:/msys64/ucrt64/bin" + os.pathsep + env.get("PATH", "")
    env.setdefault("SAIDA_WINDOW_HIDDEN", "1")
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", str(GAME / "Play.ps1"), "-BuildOnly"], check=True, env=env)
    exe = GAME / "generated" / "world-windows" / "R1World.exe"
    runs = []
    # Six metres apart along the parallel: in sight, not in each other's capsule.
    for role, east, protocol in roles:
        session = GAME / "cache" / "sessions" / uuid.uuid4().hex
        session.mkdir(parents=True)
        log = (session / "game.log").open("w", encoding="utf-8")
        spawn = lon + east / (111195.0 * max(1e-6, abs(math.cos(math.radians(lat)))))
        extra = {"R1WORLD_ONLINE_SMOKE": role, "SAIDA_CRASH_DIR": str(session)}
        if protocol:
            extra["R1WORLD_ONLINE_PROTOCOL"] = protocol
        if role == "shooter":  # his screen when his hit was confirmed (at the real hour)
            extra["R1WORLD_ONLINE_SHOT"] = str(session / "shooter-view.png")
        child = subprocess.Popen([str(exe), "--project", str(GAME), "--smoke", "--spawn", repr(spawn), repr(lat)],
                                 cwd=GAME, env={**env, **extra}, stdout=log, stderr=log)
        runs.append((role, child, log, session))
    failed = 0
    for role, child, log, session in runs:
        code = child.wait()
        log.close()
        verdict = [line.strip() for line in (session / "game.log").open(encoding="utf-8", errors="replace")
                   if "[World E2E]" in line or "[World online]" in line]
        print(f"{role}: exit {code}, logs {session}")
        for crash in session.glob("*.crash.log"):  # the engine's report: where it crashed
            print("    crashed:", crash)
            for line in crash.open(encoding="utf-8", errors="replace"):
                if line.startswith(("reason=", "exceptionCode=", "exceptionRva=")):
                    print("     ", line.strip())
        for line in verdict[-12:]:
            print("   ", line)
        failed |= code != 0 or not any("PASS online" in line for line in verdict)
    print("FAIL" if failed else "PASS", "online smoke")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
