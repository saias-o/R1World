# Play R1World from the development tree.
#
#   powershell -File game\Play.ps1 [-Rebuild] [-BuildOnly] [game arguments...]
#
# The world is generated inside the game (native/gen), so playing needs the
# executable and nothing else: no Python, no worker. The executable is built
# here too, against an optimized build of Saida (engine\build-rel,
# RelWithDebInfo), which this script configures when it is missing and brings
# up to date before every game build: the engine is developed with the game
# (CLAUDE.md §6). engine\build is the engine's Debug tree, unoptimized, and
# the game never links against it. The executable is rebuilt by itself
# whenever a source is newer than it -- a stale executable is a game that
# silently does not have the change you just made.
#
#   -Rebuild    recompile every translation unit, not only the stale ones
#   -BuildOnly  build, then stop (what the test drivers call)
#   -Build      kept for the old command line; building is automatic now
#
# Everything else is passed to the game (--smoke, --spawn lon lat, --fly, ...).
# The game's output, stderr included, goes to cache/sessions/<id>/game.log.
param([switch]$Build, [switch]$Rebuild, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'

$game = $PSScriptRoot
$native = Join-Path $game 'native'
$engineSource = Join-Path $game '..\engine'
$engineBuild = Join-Path $engineSource 'build-rel'
$out = Join-Path $game 'generated\world-windows'
$exe = Join-Path $out 'R1World.exe'
$msys = 'C:\msys64\ucrt64\bin'
$compiler = Join-Path $msys 'g++.exe'
# First on PATH, or the link fails with a bare "ld returned 116" (engine/AGENTS.md),
# and the game finds the MSYS2 runtime it was linked against.
$env:PATH = $msys + ';' + $env:PATH

# ── building ────────────────────────────────────────────────────────────────

# The game's translation units and the flags only they need. The generator is
# compiled without contracting a*b+c into one rounding: the same observations
# must give the same city on every machine (PLAN §3 I3).
function Get-Units {
    $gen = @('-ffp-contract=off')
    # world.cpp alone includes the engine's headers: an engine change that
    # moved a member must recompile it, or it and the library disagree.
    $units = @(@{ Source = (Join-Path $native 'world.cpp'); Extra = @(); Engine = $true })
    foreach ($dir in @('gen', 'third_party\clipper2\src')) {
        Get-ChildItem -LiteralPath (Join-Path $native $dir) -Filter '*.cpp' | Sort-Object Name |
            ForEach-Object { $units += @{ Source = $_.FullName; Extra = $gen } }
    }
    return $units
}

# The newest header any unit may include: touching one rebuilds them all.
function Get-HeaderTime {
    $newest = [datetime]::MinValue
    Get-ChildItem -LiteralPath (Join-Path $native 'gen') -Filter '*.hpp' |
        ForEach-Object { if ($_.LastWriteTime -gt $newest) { $newest = $_.LastWriteTime } }
    return $newest
}

function Test-Stale([string]$target, [string]$source, [datetime]$headers) {
    if (-not (Test-Path -LiteralPath $target)) { return $true }
    $built = (Get-Item -LiteralPath $target).LastWriteTime
    return ((Get-Item -LiteralPath $source).LastWriteTime -gt $built) -or ($headers -gt $built)
}

# Whether the executable is older than anything it is made of.
function Test-GameStale {
    if (-not (Test-Path -LiteralPath $exe)) { return $true }
    $built = (Get-Item -LiteralPath $exe).LastWriteTime
    if ((Get-HeaderTime) -gt $built) { return $true }
    foreach ($unit in Get-Units) {
        if ((Get-Item -LiteralPath $unit.Source).LastWriteTime -gt $built) { return $true }
    }
    $engineLib = Join-Path $engineBuild 'libsaida_engine.a'
    return (Test-Path -LiteralPath $engineLib) -and ((Get-Item -LiteralPath $engineLib).LastWriteTime -gt $built)
}

# A ninja variable's value split as a POSIX shell would (shlex): the engine's
# flags quote paths with spaces and escape the quotes of string defines.
function Split-Flags([string]$text) {
    $words = New-Object System.Collections.Generic.List[string]
    $word = New-Object System.Text.StringBuilder
    $inWord = $false
    $i = 0
    while ($i -lt $text.Length) {
        $c = $text[$i]
        if ($c -eq '"') {
            $inWord = $true; $i++
            while ($i -lt $text.Length -and $text[$i] -ne '"') {
                if ($text[$i] -eq '\' -and $i + 1 -lt $text.Length -and '"\$`'.IndexOf($text[$i + 1]) -ge 0) { $i++ }
                [void]$word.Append($text[$i]); $i++
            }
            $i++
        } elseif ($c -eq "'") {
            $inWord = $true; $i++
            while ($i -lt $text.Length -and $text[$i] -ne "'") { [void]$word.Append($text[$i]); $i++ }
            $i++
        } elseif ($c -eq '\' -and $i + 1 -lt $text.Length) {
            $inWord = $true; [void]$word.Append($text[$i + 1]); $i += 2
        } elseif ([char]::IsWhiteSpace($c)) {
            if ($inWord) { $words.Add($word.ToString()); [void]$word.Clear(); $inWord = $false }
            $i++
        } else {
            $inWord = $true; [void]$word.Append($c); $i++
        }
    }
    if ($inWord) { $words.Add($word.ToString()) }
    return , $words.ToArray()
}

# The compile and link flags of the engine's own runtime, read from its
# generated Ninja file, so the game keeps the engine's ABI (GLM, Jolt, XR).
function Get-EngineFlags {
    $ninja = Join-Path $engineBuild 'build.ninja'
    if (-not (Test-Path -LiteralPath $ninja)) { throw "Build du moteur introuvable : $ninja" }
    $text = [IO.File]::ReadAllText($ninja) -replace "`r`n", "`n"
    $compile = [regex]::Match($text, '(?s)build CMakeFiles/SaidaEngineRuntime\.dir/src/runtime/main\.cpp\.obj:.*?\n(.*?)(?=\n\n)')
    $link = [regex]::Match($text, '(?s)build bin/SaidaEngineRuntime\.exe:.*?\n(.*?)(?=\n\n)')
    if (-not $compile.Success -or -not $link.Success) { throw "build.ninja ne décrit plus SaidaEngineRuntime : lien impossible" }
    $value = {
        param($block, $key)
        $m = [regex]::Match($block, "(?m)^  $key = (.*)$")
        if (-not $m.Success) { throw "build.ninja : $key absent" }
        return (Split-Flags $m.Groups[1].Value)
    }
    $body = $compile.Groups[1].Value
    $includes = @("-I$(Join-Path $game '..\engine\plugins\traffic\include')", "-I$native",
                  "-I$(Join-Path $native 'third_party\clipper2\include')", "-I$(Join-Path $native 'third_party\earcut')")
    return @{
        Compile = @(& $value $body 'DEFINES') + @(& $value $body 'INCLUDES') + $includes + @(& $value $body 'FLAGS') + @('-O2')
        Link = @(& $value $link.Groups[1].Value 'LINK_FLAGS') + @(& $value $link.Groups[1].Value 'LINK_LIBRARIES')
    }
}

# A GCC response file: every argument quoted, so no path with a space and no
# quoted define goes through PowerShell's own argument quoting.
function Write-Response([string]$path, [string[]]$arguments) {
    $lines = $arguments | ForEach-Object { '"' + ($_ -replace '\\', '\\' -replace '"', '\"') + '"' }
    [IO.File]::WriteAllLines($path, [string[]]$lines)
}

function Start-Compiler([string]$response, [string]$errors) {
    $p = Start-Process -FilePath $compiler -ArgumentList ('"@' + $response + '"') -WorkingDirectory $engineBuild `
        -NoNewWindow -PassThru -RedirectStandardError $errors
    $null = $p.Handle  # keeps the exit code readable once it has exited
    return $p
}

function Invoke-Build([bool]$everything) {
    if (-not (Test-Path -LiteralPath $compiler)) { throw "Compilateur introuvable : $compiler (MSYS2 UCRT64)" }
    $flags = Get-EngineFlags
    $objects = Join-Path $out 'obj'
    New-Item -ItemType Directory -Force -Path $objects | Out-Null
    $headers = Get-HeaderTime
    $queue = New-Object System.Collections.Queue
    $all = @()
    foreach ($unit in Get-Units) {
        $obj = Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($unit.Source) + '.o')
        $all += $obj
        $newest = $headers
        $engineLib = Join-Path $engineBuild 'libsaida_engine.a'
        if ($unit.Engine -and (Test-Path -LiteralPath $engineLib)) {
            $built = (Get-Item -LiteralPath $engineLib).LastWriteTime
            if ($built -gt $newest) { $newest = $built }
        }
        if ($everything -or (Test-Stale $obj $unit.Source $newest)) {
            $queue.Enqueue(@{ Source = $unit.Source; Object = $obj; Extra = $unit.Extra })
        }
    }
    $compiled = $queue.Count
    $running = @()
    $failed = @()
    $slots = [Math]::Max(1, [Environment]::ProcessorCount)
    while ($queue.Count -gt 0 -or $running.Count -gt 0) {
        while ($queue.Count -gt 0 -and $running.Count -lt $slots) {
            $job = $queue.Dequeue()
            $name = [IO.Path]::GetFileNameWithoutExtension($job.Object)
            $response = Join-Path $objects "$name.rsp"
            Write-Response $response ($flags.Compile + $job.Extra + @('-c', $job.Source, '-o', $job.Object))
            Write-Host "  compile $name"
            $running += @{ Process = (Start-Compiler $response (Join-Path $objects "$name.err")); Job = $job; Name = $name }
        }
        Start-Sleep -Milliseconds 100
        $still = @()
        foreach ($r in $running) {
            if (-not $r.Process.HasExited) { $still += $r; continue }
            $r.Process.WaitForExit()
            if ($r.Process.ExitCode -ne 0) { $failed += $r }
        }
        $running = $still
    }
    if ($failed.Count -gt 0) {
        foreach ($r in $failed) {
            Write-Host "── $($r.Name) ──" -ForegroundColor Red
            Get-Content -LiteralPath (Join-Path $objects "$($r.Name).err") | Select-String -Pattern 'error' | Select-Object -First 20 |
                ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
            if (Test-Path -LiteralPath $r.Job.Object) { Remove-Item -LiteralPath $r.Job.Object }
        }
        throw "Compilation échouée : $(($failed | ForEach-Object { $_.Name }) -join ', ')"
    }
    $response = Join-Path $objects 'link.rsp'
    Write-Response $response ($all + $flags.Link + @('-lwinhttp', '-o', $exe))
    Write-Host "  link R1World.exe ($compiled unité(s) recompilée(s))"
    $linker = Start-Compiler $response (Join-Path $objects 'link.err')
    $linker.WaitForExit()
    if ($linker.ExitCode -ne 0) {
        Get-Content -LiteralPath (Join-Path $objects 'link.err') | Select-Object -Last 20 | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        throw "Édition de liens échouée (code $($linker.ExitCode))"
    }
    Copy-Item -LiteralPath (Join-Path $msys 'glfw3.dll') -Destination $out -Force
}

# The engine, optimized and up to date. Ninja does nothing when nothing
# changed, and rebuilds only what an engine change touched.
function Update-Engine {
    $cmake = Join-Path $msys 'cmake.exe'
    if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake introuvable : $cmake (MSYS2 UCRT64)" }
    $env:PATH = $msys + ';C:\msys64\usr\bin;' + $env:PATH
    if (-not (Test-Path -LiteralPath (Join-Path $engineBuild 'build.ninja'))) {
        Write-Host 'Configuration du moteur optimisé (engine\build-rel)...'
        & $cmake -S $engineSource -B $engineBuild -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Configuration du moteur échouée" }
    }
    # MSYS2's compiler needs a temporary directory it can write to.
    $tmp = Join-Path $engineBuild 'tmp'
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $env:TMP = $tmp; $env:TEMP = $tmp
    $output = & $cmake --build $engineBuild --target SaidaEngineRuntime --parallel 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        $output | Select-String -Pattern 'error|FAILED' | Select-Object -First 20 | ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
        throw "Compilation du moteur échouée"
    }
    if ($output -match 'Linking') { Write-Host '  moteur mis à jour' }
}

if (Test-Path -LiteralPath $compiler) { Update-Engine }
if ($Rebuild -or (Test-GameStale)) {
    if (-not (Test-Path -LiteralPath $compiler) -and (Test-Path -LiteralPath $exe) -and -not $Rebuild) {
        # Said, not silent: playing an executable older than its sources is
        # playing without the latest change.
        Write-Warning "Sources plus récentes que l'exécutable et compilateur absent : lancement de l'exécutable existant."
    } else {
        Write-Host 'Compilation de R1World...'
        Invoke-Build ([bool]$Rebuild)
    }
}
if ($BuildOnly) { exit 0 }
if (-not (Test-Path -LiteralPath $exe)) { throw "Jeu non compilé : $exe" }

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
