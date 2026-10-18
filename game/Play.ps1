# Build and run this project through the engine's native CMake SDK.
param([switch]$Build, [switch]$Rebuild, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$game = $PSScriptRoot
$engineSource = Join-Path $game '..\engine'
$engineBuild = Join-Path $engineSource 'build-rel'
$gameBuild = Join-Path $game 'generated\world-windows\build'
$exe = Join-Path $game 'generated\world-windows\R1World.exe'
$msys = 'C:\msys64\ucrt64\bin'
$env:PATH = $msys + ';C:\msys64\usr\bin;' + $env:PATH
$cmake = Join-Path $msys 'cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake introuvable : $cmake (MSYS2 UCRT64)" }
# Keep compilation bounded on the 7 GB reference machine.
if ($env:R1_BUILD_JOBS) {
    $buildJobs = [Math]::Max(1, [int]$env:R1_BUILD_JOBS)
} else {
    try {
        $ramGB = (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory / 1GB
        $buildJobs = [Math]::Max(1, [Math]::Min([Math]::Min(4, [Environment]::ProcessorCount), [int][Math]::Floor($ramGB / 1.5)))
    } catch {
        $buildJobs = [Math]::Max(1, [Math]::Min(2, [Environment]::ProcessorCount))
    }
}
$tmp = Join-Path $engineBuild 'tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp; $env:TEMP = $tmp
if (-not (Test-Path -LiteralPath (Join-Path $engineBuild 'CMakeCache.txt'))) {
    & $cmake -S $engineSource -B $engineBuild -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    if ($LASTEXITCODE -ne 0) { throw 'Configuration du moteur échouée' }
}
# CMake tracks engine headers, dependencies and its package configuration.
& $cmake --build $engineBuild --target SaidaEngineRuntime --parallel $buildJobs
if ($LASTEXITCODE -ne 0) { throw 'Compilation du moteur échouée' }
$sdk = Join-Path $engineBuild 'sdk'
& $cmake -S $game -B $gameBuild -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DSaidaEngine_DIR=$sdk"
if ($LASTEXITCODE -ne 0) { throw 'Configuration de R1World échouée' }
if ($Rebuild) {
    & $cmake --build $gameBuild --target clean
    if ($LASTEXITCODE -ne 0) { throw 'Nettoyage du build de R1World échoué' }
}
& $cmake --build $gameBuild --target R1World --parallel $buildJobs
if ($LASTEXITCODE -ne 0) { throw 'Compilation de R1World échouée' }
if ($BuildOnly) { exit 0 }

# ── playing ─────────────────────────────────────────────────────────────────

$session = Join-Path $game ('cache\sessions\' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $session | Out-Null
$log = Join-Path $session 'game.log'
# The engine writes its warnings to stderr. Windows PowerShell turns each
# stderr line of a native program into an error record, and under 'Stop' the
# first one ("validation layers requested but not available") ended this
# script before the game had drawn a frame. Both streams go to the log, as text.
$ErrorActionPreference = 'Continue'
& $exe --project $game @args 2>&1 | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding UTF8
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host "R1World s'est arrêté avec le code $code :" -ForegroundColor Red
    Get-Content -LiteralPath $log | Select-String -Pattern '\[error\]|FAIL|R1World:' | Select-Object -Last 10 |
        ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
}
Write-Host "Logs: $session"
exit $code
