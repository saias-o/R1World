param([switch]$Build)
$ErrorActionPreference = 'Stop'
# The game ships its own isolated runtime. Never choose an interpreter from
# PATH: shell profiles, Conda and MSYS must not change the game's dependencies.
$worldPython = Join-Path $PSScriptRoot 'generated/python-runtime/python.exe'
if (-not (Test-Path -LiteralPath $worldPython)) {
    throw 'Runtime du jeu absent : generated/python-runtime/python.exe'
}
if ($Build) {
    & $worldPython (Join-Path $PSScriptRoot 'tools/build_world.py')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& $worldPython (Join-Path $PSScriptRoot 'tools/play_world.py') @args
exit $LASTEXITCODE
