# R1World

A free open-world game for exploring the entire Earth at 1:1 scale, designed
with modest PCs in mind. Powered by SaidaEngine, it generates the world locally
from real geographic data.

The current game version is **0.1**, with its release coming soon. The version
is also shown in Settings → Credits in the game.

## Features

- Photograph the world and keep a persistent portfolio; open the player menu
  with M to view photos, money, inventory and ammunition.
- Buildings placed at their real-world locations.
- Explorable building interiors.
- Drive cars and motorcycles, sail boats, and fly planes and helicopters.
- Weather driven by real-world meteorological data.

## Requirements

- Windows 11 x64 and a GPU with Vulkan 1.3 support and up-to-date drivers.
- [MSYS2](https://www.msys2.org/) installed at `C:\msys64` (required by the launcher).
- Git, Git LFS, GCC, CMake 3.20+, Ninja, GLFW, GLM and Vulkan tools, installed below.

In the **MSYS2 UCRT64** terminal, update MSYS2 (restart if prompted), then install:

```sh
pacman -Syu
pacman -S --needed git git-lfs \
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-glfw mingw-w64-ucrt-x86_64-glm \
  mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-vulkan-headers \
  mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-vulkan-validation-layers
```

The remaining libraries are included in the repository or its submodules.
Python and the LunarG Vulkan SDK are not required to build or play.

## Get the source

In **MSYS2 UCRT64**:

```sh
git lfs install
git clone --recurse-submodules https://github.com/saias-o/r1world.git
cd r1world
git lfs pull
git -C engine lfs pull
```

For an existing checkout, run `git submodule update --init --recursive` first.

## Build and play

From the repository root in **PowerShell**:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File game\Play.ps1 -BuildOnly
.\Play.cmd
```

The launcher builds both SaidaEngine and R1World using `RelWithDebInfo`, then
starts the game unless `-BuildOnly` is set. You can also double-click `Play.cmd`.
The game executable is `game/generated/world-windows/R1World.exe`.
An internet connection fetches geographic data for new locations; visited areas
are cached locally.

## Build the engine separately

From the repository root in **MSYS2 UCRT64**:

```sh
cmake -S engine -B engine/build-rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build engine/build-rel --target SaidaEngine SaidaEngineHub SaidaEngineRuntime saida_tool --parallel 2
```

Executables are written to `engine/build-rel/bin/`.
See the [game documentation](game/README.md) for controls and technical details,
or the [engine documentation](engine/README.md) for additional build options.

## License

[GPL-3.0](LICENSE). Third-party data and assets retain their own licenses;
see the [attributions](game/README.md#attribution).
