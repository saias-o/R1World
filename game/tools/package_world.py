"""R1World as a folder another machine can run: the engine's export, stripped.

    python tools/package_world.py [--partial-relief]

Builds (`Play.ps1 -BuildOnly`), then exports with the engine's own
`saida_tool export-game`, exactly as the editor's Build does: R1World.exe as
the runtime, beside its boot manifest (game.saida), the engine's shaders and
fonts, and the project's assets, scenes, scripts and ui. R1World.exe finds
them there by itself (native/world.cpp). The executable is stripped of its
debug information first (the export then writes its version into it, which
binutils cannot read back): the unstripped one stays in
generated/world-windows and reads the addresses of the crash reports this one
writes. Then the menu globe's shaders go in, and the folder is zipped:

    generated/package/R1World-<commit>.zip

Unzip it anywhere writable and run "Jouer.cmd" (R1World.exe, its journal
written to logs/), or R1World.exe itself: cache/ and saves/ are made beside
it. LISEZMOI.txt says how to join a friend and what to send back. Nothing of this machine's cache, saves or online identity goes in.
The other machine needs a Vulkan driver (any recent GPU driver) and nothing
else: the executable carries its C++ runtime.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

GAME = Path(__file__).resolve().parents[1]
ENGINE = GAME.parent / "engine"
MSYS = Path("C:/msys64/ucrt64/bin")


LAUNCHER = r"""@echo off
rem R1World, its journal kept in logs\ (one file a session). Arguments pass on: Jouer.cmd --join K7QM-2XPA
setlocal
cd /d "%~dp0"
set SAIDA_CRASH_DIR=%~dp0crash
if not exist logs mkdir logs
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set stamp=%%t
echo R1World : journal dans logs\game-%stamp%.log
"%~dp0R1World.exe" %* > "logs\game-%stamp%.log" 2>&1
"""

README = r"""{name}

Lancer : double-cliquer sur Jouer.cmd. Le journal de la partie est écrit dans
logs\ (un fichier par partie). R1World.exe se lance aussi seul, sans journal.

Au premier lancement, Windows peut afficher « Windows a protégé votre
ordinateur » (exécutable non signé) : « Informations complémentaires », puis
« Exécuter quand même ». Le pare-feu demande ensuite l'accès au réseau :
l'autoriser, sinon les autres joueurs ne vous trouvent pas.

Il faut un pilote graphique récent (Vulkan). Rien d'autre à installer.

Rejoindre un ami :
  - lui : Settings > Online, lire son code (par exemple K7QM-2XPA) ;
  - vous : Settings > Online, taper ce code, Join. Ou lancer
    Jouer.cmd --join K7QM-2XPA
  Le jeu vous emmène à côté de lui. Le panneau en bas dit où en est la recherche.

Jouer seul, sans réseau : Jouer.cmd --offline

En ligne, votre adresse réseau est visible des joueurs que vous rencontrez.

À renvoyer après une session : le fichier de logs\ de la session, et le
dossier crash\ s'il existe.
"""


def main():
    # Without the installed relief layer a first visit stands on flat ground
    # at sea level until its survey lands, and the far relief is refused
    # (README, "installed relief"): no package goes out without it.
    relief = GAME / "assets" / "world" / "relief"
    packs = list(relief.glob("*.r1relief")) if relief.is_dir() else []
    if len(packs) < 648 and "--partial-relief" not in sys.argv:
        print(f"FAIL package: {relief} holds {len(packs)} of 648 relief packs; build them with "
              "generated/tools/r1relief.exe --out assets/world/relief (or --partial-relief to pack them anyway)")
        return 1
    env = {**os.environ, "PATH": str(MSYS) + os.pathsep + os.environ.get("PATH", "")}
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", str(GAME / "Play.ps1"), "-BuildOnly"], check=True, env=env)
    built = GAME / "generated" / "world-windows"
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=GAME, capture_output=True,
                            text=True, check=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain", "--", "native", "ui", "scenes", "scripts", "shaders"],
                           cwd=GAME, capture_output=True, text=True, check=True).stdout.strip()
    name = f"R1World-{commit}{'-modified' if dirty else ''}"
    root = GAME / "generated" / "package"
    out = root / name
    if out.exists():
        shutil.rmtree(out)  # an earlier package of this same build, made by this tool
    root.mkdir(parents=True, exist_ok=True)
    runtime = root / "R1World-stripped.exe"
    subprocess.run([str(MSYS / "strip.exe"), "--strip-debug", "-o", str(runtime), str(built / "R1World.exe")], check=True)
    # What R1World.exe imports beyond Windows itself must be in the folder.
    imports = subprocess.run([str(MSYS / "objdump.exe"), "-p", str(runtime)], capture_output=True, text=True,
                             check=True).stdout
    tool = ENGINE / "build-rel" / "bin" / "saida_tool.exe"
    subprocess.run([str(tool), "export-game", str(GAME / "R1World.saidaproj"), "--platform", "windows",
                    "--runtime", str(runtime), "--main-scene", "scenes/earth.scene",
                    "--version", "0.1.0", "--out", str(out)], check=True, env=env)
    runtime.unlink()
    shutil.copytree(built / "menu-shaders", out / "menu-shaders")
    # The journal: what Play.ps1 keeps in cache/sessions, here in logs/.
    (out / "Jouer.cmd").write_text(LAUNCHER, encoding="ascii", newline="\r\n")
    (out / "LISEZMOI.txt").write_text(README.format(name=name), encoding="utf-8-sig", newline="\r\n")
    for line in imports.splitlines():
        if "DLL Name:" not in line:
            continue
        dll = line.split("DLL Name:")[1].strip()
        system = dll.lower().startswith("api-ms-win-") or dll.lower() == "vulkan-1.dll" or \
            (Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32" / dll).exists()
        if not system and not (out / dll).exists():
            print(f"FAIL package: {dll} is imported and not in the package")
            return 1
    archive = root / f"{name}.zip"
    if archive.exists():
        archive.unlink()  # the same build's earlier zip, made by this tool
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(out.rglob("*")):
            if path.is_file():
                z.write(path, Path(name) / path.relative_to(out))
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"PASS package: {out} ({size / 2**20:.0f} MB), {archive} ({archive.stat().st_size / 2**20:.0f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
