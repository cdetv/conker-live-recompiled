<#
.SYNOPSIS
    Runs the xboxrecomp pipeline on Conker's default.xbe: parse -> disassemble ->
    identify -> ABI -> generate C into src/recomp/gen/ (SPEC section 4).

.DESCRIPTION
    Stops at the first failing step. All in-between files go to _work/pipeline/<step>/;
    nothing is written into the toolkit or the (read-only) extracted game folder.
    The log goes to _work/logs/pipeline_<timestamp>.log.

    Step 1 (extract the disc) is done once by hand and is not repeated here.
    Step 6 (Ghidra names) is not wired in yet.
    Functions listed in seeds/trace_functions.json get entry tracing ([TRACE] lines).

.PARAMETER From
    Start at this step and reuse earlier outputs: parse, disasm, funcid, abi, recomp.

.PARAMETER Force
    Make the disassembler ignore its cache.

.PARAMETER GameDir
    The folder holding the game's default.xbe (the extracted disc, or the game\ folder
    the launcher imported). Default: $env:CONKER_GAME_DIR, else the CONKER_GAME_DIR
    given to CMake (build\CMakeCache.txt).

.EXAMPLE
    scripts\pipeline.ps1
    scripts\pipeline.ps1 -From recomp
    scripts\pipeline.ps1 -GameDir D:\Games\Conker\game
#>
param(
    [ValidateSet('parse', 'disasm', 'funcid', 'abi', 'recomp')]
    [string]$From = 'parse',
    [switch]$Force,
    [string]$GameDir = $env:CONKER_GAME_DIR
)

$ErrorActionPreference = 'Stop'

$Repo    = Split-Path -Parent $PSScriptRoot
$Toolkit = Join-Path $Repo 'xboxrecomp'
$Python  = Join-Path $Repo '.venv\Scripts\python.exe'
if (-not $GameDir) {
    $Cache = Join-Path $Repo 'build\CMakeCache.txt'
    if (Test-Path $Cache) {
        $m = Select-String -Path $Cache -Pattern '^CONKER_GAME_DIR:[A-Z]+=(.+)$' | Select-Object -First 1
        if ($m) { $GameDir = $m.Matches[0].Groups[1].Value }
    }
}
if (-not $GameDir) { throw 'Pass -GameDir <folder with default.xbe> (or set CONKER_GAME_DIR).' }
$Xbe     = Join-Path $GameDir 'default.xbe'
$Work    = Join-Path $Repo '_work\pipeline'
$GenDir  = Join-Path $Repo 'src\recomp\gen'
$Seeds   = @(Join-Path $Repo 'seeds\hand_seeds.json')
# Functions that log their entry registers ([TRACE] lines) for comparison with xemu.
# Keep the list short: each entry costs a log line per call.
$TraceList = Join-Path $Repo 'seeds\trace_functions.json'

$ParseDir  = Join-Path $Work 'parse'
$DisasmDir = Join-Path $Work 'disasm'
$FuncIdDir = Join-Path $Work 'func_id'
$AbiDir    = Join-Path $Work 'abi'
$RecompDir = Join-Path $Work 'recomp'
$Analysis  = Join-Path $ParseDir 'conker_analysis.json'

$LogDir = Join-Path $Repo '_work\logs'
New-Item -ItemType Directory -Force $LogDir, $ParseDir, $DisasmDir, $FuncIdDir, $AbiDir, $RecompDir | Out-Null
$Log = Join-Path $LogDir ("pipeline_{0}.log" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))

foreach ($p in @($Python, $Xbe, (Join-Path $Toolkit 'tools'))) {
    if (-not (Test-Path $p)) { throw "Missing: $p" }
}

$Steps = 'parse', 'disasm', 'funcid', 'abi', 'recomp'
$StartIndex = [array]::IndexOf($Steps, $From)

function Invoke-Step([string]$Name, [string[]]$ToolArgs) {
    if ([array]::IndexOf($Steps, $Name) -lt $StartIndex) {
        Write-Host "--- $Name (skipped, reusing earlier output)"
        return
    }
    $line = "=== $Name : python -m $($ToolArgs -join ' ')"
    Write-Host $line -ForegroundColor Cyan
    Add-Content $Log $line
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $Python -m @ToolArgs 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $Log -Append
    $code = $LASTEXITCODE
    $done = "=== $Name finished in {0:N0}s, exit code {1}" -f $timer.Elapsed.TotalSeconds, $code
    Write-Host $done -ForegroundColor Cyan
    Add-Content $Log $done
    if ($code -ne 0) { throw "Step '$Name' failed (exit code $code). Log: $Log" }
}

# The tools are run as modules from inside the toolkit (tools.<name>).
Push-Location $Toolkit
try {
    Invoke-Step 'parse' @('tools.xbe_parser', $Xbe, '--json', $Analysis, '--quiet')

    # Every executable section, not just .text: the XDK libraries (D3D, DSOUND, XPP, ...)
    # call into themselves, and with --text-only only functions that game code calls
    # directly were found. D3D's device init (0x0053E5D0) was a do-nothing stub that way.
    $disasmArgs = @('tools.disasm', $Xbe, '--analysis-json', $Analysis, '-o', $DisasmDir, '-v')
    foreach ($s in $Seeds) { $disasmArgs += @('--seed-functions', $s) }
    if ($Force) { $disasmArgs += '--force' }
    Invoke-Step 'disasm' $disasmArgs

    Invoke-Step 'funcid' @('tools.func_id', $Xbe,
        '--functions', (Join-Path $DisasmDir 'functions.json'),
        '--strings',   (Join-Path $DisasmDir 'strings.json'),
        '--xrefs',     (Join-Path $DisasmDir 'xrefs.json'),
        '-o', $FuncIdDir, '-v')

    # Required: without it every function silently becomes cdecl with 0 parameters.
    Invoke-Step 'abi' @('tools.abi_analysis', $Xbe, '--disasm-dir', $DisasmDir,
        '--func-id-dir', $FuncIdDir, '--output-dir', $AbiDir, '-v')

    # Start from an empty gen folder so no stale split files survive a regeneration.
    if ([array]::IndexOf($Steps, 'recomp') -ge $StartIndex -and (Test-Path $GenDir)) {
        Remove-Item -Recurse -Force $GenDir
    }
    $recompArgs = @('tools.recomp', $Xbe, '--game-name', 'Conker', '--all',
        '--split', '250', '--disasm-dir', $DisasmDir, '--func-id-dir', $FuncIdDir,
        '--abi-dir', $AbiDir, '-o', $RecompDir, '--gen-dir', $GenDir,
        # Functions defined by hand in recomp_manual.c are not generated.
        '--exclude-manual', (Join-Path $Repo 'src\recomp_manual.c'))
    if (Test-Path $TraceList) { $recompArgs += @('--trace-functions', $TraceList) }
    Invoke-Step 'recomp' $recompArgs
}
finally {
    Pop-Location
}

# Every step was given its folders explicitly, so the toolkit's default output folders
# must stay empty. Anything there means a flag was missed (architecture.md, gotchas).
$stray = Get-ChildItem (Join-Path $Toolkit 'tools') -Directory -Filter output -Recurse -ErrorAction SilentlyContinue |
    Where-Object { Get-ChildItem $_.FullName -File -Recurse }
if ($stray) {
    Write-Warning ("Toolkit output folders are not empty: " + ($stray.FullName -join ', '))
}

Write-Host "Pipeline done. Generated C: $GenDir  Log: $Log" -ForegroundColor Green
