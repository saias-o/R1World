param([switch]$Build)
$ErrorActionPreference = 'Stop'
# The world is generated inside the game (native/gen): playing needs the
# executable and nothing else. Only building it uses the tools' Python.
if ($Build) {
    & (Join-Path $PSScriptRoot 'generated/python-runtime/python.exe') (Join-Path $PSScriptRoot 'tools/build_world.py')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
$exe = @('generated/world-windows/R1World.exe', 'generated/world-windows-next/R1World.exe') |
    ForEach-Object { Join-Path $PSScriptRoot $_ } | Where-Object { Test-Path -LiteralPath $_ } |
    Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending | Select-Object -First 1
if (-not $exe) { throw 'Jeu non compilé : lancez Play.ps1 -Build' }
# A development build links against the MSYS2 runtime it was compiled with.
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
$session = Join-Path $PSScriptRoot ('cache/sessions/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $session | Out-Null
& $exe --project $PSScriptRoot @args *> (Join-Path $session 'game.log')
Write-Host "Logs: $session"
exit $LASTEXITCODE
