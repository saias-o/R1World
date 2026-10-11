# Exercises the actual game vehicle setup against Saida's solver, without a GPU.
$ErrorActionPreference='Stop'
$game=Split-Path -Parent $PSScriptRoot
& (Join-Path $game 'Play.ps1') -BuildOnly
if($LASTEXITCODE -ne 0){throw 'Game build failed'}
$taskOutput=Join-Path $game 'generated\vehicle-validation'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$objectDirectory=Join-Path $game 'generated\world-windows\obj'
$testSource=Join-Path $game 'native\tests_engine\vehicle_runtime_test.cpp'
$testObject=Join-Path $taskOutput 'vehicle-runtime.o'
$testExe=Join-Path $taskOutput 'vehicle-runtime.exe'
$compile=(Get-Content -LiteralPath (Join-Path $objectDirectory 'world.rsp') -Raw).
    Replace((Join-Path $game 'native\world.cpp').Replace('\','\\'),$testSource.Replace('\','/')).
    Replace((Join-Path $objectDirectory 'world.o').Replace('\','\\'),$testObject.Replace('\','/'))
$compileResponse=Join-Path $taskOutput 'vehicle-runtime-compile.rsp'
$linkResponse=Join-Path $taskOutput 'vehicle-runtime-link.rsp'
[IO.File]::WriteAllText($compileResponse,$compile)
$link=@('"'+$testObject.Replace('\','/')+'"')
foreach($line in Get-Content -LiteralPath (Join-Path $objectDirectory 'link.rsp')) {
    if($line -match '\.o"$'){continue}
    $link+=$line.Replace((Join-Path $game 'generated\world-windows\R1World.exe').Replace('\','\\'),$testExe.Replace('\','/'))
}
[IO.File]::WriteAllLines($linkResponse,$link)
$env:PATH='C:\msys64\ucrt64\bin;'+$env:PATH
Push-Location (Join-Path $game '..\engine\build-rel')
try {
    & g++ ('@'+$compileResponse)
    if($LASTEXITCODE -ne 0){throw 'Vehicle test compilation failed'}
    & g++ ('@'+$linkResponse)
    if($LASTEXITCODE -ne 0){throw 'Vehicle test linking failed'}
    & $testExe
    if($LASTEXITCODE -ne 0){throw 'Vehicle runtime regression'}
} finally {Pop-Location}
