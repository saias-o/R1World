# Exercises the actual game vehicle setup against Saida's solver, without a GPU.
param([switch]$MotorcyclesOnly)
$ErrorActionPreference='Stop'
$game=Split-Path -Parent $PSScriptRoot
& (Join-Path $game 'Play.ps1') -BuildOnly
if($LASTEXITCODE -ne 0){throw 'Game build failed'}
$taskOutput=Join-Path $game 'generated\vehicle-validation'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$testExe=Join-Path $game 'generated\native-tests\r1_vehicle_runtime_test.exe'
$env:PATH='C:\msys64\ucrt64\bin;'+$env:PATH
Push-Location $game
try {
    & cmake --build generated/world-windows/build --target r1_vehicle_runtime_test --parallel 2
    if($LASTEXITCODE -ne 0){throw 'Vehicle test compilation failed'}
    if($MotorcyclesOnly){& $testExe --motorcycles}else{& $testExe}
    if($LASTEXITCODE -ne 0){throw 'Vehicle runtime regression'}
} finally {Pop-Location}
