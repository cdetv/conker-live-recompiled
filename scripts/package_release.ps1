#Requires -Version 7
<#
.SYNOPSIS
    Builds the release zip conker-live-recompiled-<version>.zip (SPEC 9.1) into
    _work\release\<version>\.

.DESCRIPTION
    1. Builds conker.exe (cmake --build build --config Release; the build folder must be
       configured already, see CLAUDE.md) and publishes conker-launcher.exe
       (dotnet publish launcher). Starts a VS dev shell for the C build when this shell
       isn't one.
    2. Checks both .exe files carry the version in the repo's VERSION file.
    3. Copies only the files a release ships into a staging folder: conker.exe,
       xemu_apu.dll (the xemu audio emulation, LGPL: SPEC 9.5), conker-launcher.exe (not
       the .pdb files dotnet publish drops next to it), the title
       logo and the cheat words (fire_imps.journal) in textures\, LICENSE.txt,
       THIRD_PARTY_NOTICES.txt, README.txt, DEBUG_MODE.txt (release\DEBUG_MODE.md).
    4. Zips it and checks the zip holds exactly that list: never settings.ini or anything
       else from a save folder (SPEC 9.6), never game\ or decensor\ (SPEC 9.1).

    The launcher's updater replaces the files this zip holds and keeps everything else
    in the player's folder, so a file added here reaches players on their next update.

.PARAMETER SkipBuild
    Package what build\Release and the last publish hold, without building.

.PARAMETER Draft
    Allows LICENSE.txt, THIRD_PARTY_NOTICES.txt, README.txt and DEBUG_MODE.txt to be missing (the private
    test zip before they exist). A public release never uses it.

.EXAMPLE
    pwsh scripts\package_release.ps1
    pwsh scripts\package_release.ps1 -Draft -SkipBuild
#>
param(
    [switch]$SkipBuild,
    [switch]$Draft
)

$ErrorActionPreference = 'Stop'

$Repo    = Split-Path -Parent $PSScriptRoot
$Version = (Get-Content -LiteralPath (Join-Path $Repo 'VERSION') -TotalCount 1).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION holds '$Version', not major.minor.patch" }

$OutDir  = Join-Path $Repo "_work\release\$Version"
$PubDir  = Join-Path $OutDir 'publish'
$Stage   = Join-Path $OutDir 'stage'
$ZipName = "conker-live-recompiled-$Version.zip"
$Zip     = Join-Path $OutDir $ZipName
$GameExe = Join-Path $Repo 'build\Release\conker.exe'
$Logo    = Join-Path $Repo 'build\Release\textures\E761496A53AD4678_title_logo.png'

# What the zip holds (published path -> source). Docs come from the repo root.
$Files = [ordered]@{
    'conker.exe'                                   = $GameExe
    'xemu_apu.dll'                                 = Join-Path $Repo 'build\Release\xemu_apu.dll'
    'conker-launcher.exe'                          = Join-Path $PubDir 'conker-launcher.exe'
    'textures\E761496A53AD4678_title_logo.png'     = $Logo
    # Every cheat word, tucked away (user 2026-11-08, SPEC 10.3): the words are game text, so the
    # file stays out of git; only *.png in textures\ are read as textures. Named .journal, not .txt,
    # so it doesn't stand out (user 2026-11-11; the release notes' riddle: "help him find his journal").
    'textures\fire_imps.journal'                   = Join-Path $Repo '_work\research\cheat_words_release.txt'
    'LICENSE.txt'                                  = Join-Path $Repo 'LICENSE.txt'
    'THIRD_PARTY_NOTICES.txt'                      = Join-Path $Repo 'THIRD_PARTY_NOTICES.txt'
    'README.txt'                                   = Join-Path $Repo 'README.txt'
    # The public debug mode page (SPEC 10.3), Markdown read as plain text (user 2026-11-11).
    'DEBUG_MODE.txt'                               = Join-Path $Repo 'release\DEBUG_MODE.md'
}
$Docs = 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'README.txt', 'DEBUG_MODE.txt'

function Invoke-Checked([string]$What, [scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

# ── 1. Build ─────────────────────────────────────────────────────────────────
if (-not $SkipBuild) {
    if (-not (Test-Path -LiteralPath (Join-Path $Repo 'build\CMakeCache.txt'))) {
        throw 'build\ is not configured: run the cmake -S . -B build ... line from CLAUDE.md first'
    }

    Write-Host "Building conker.exe $Version"
    if ($env:VSCMD_VER) {
        Invoke-Checked 'cmake --build' { cmake --build (Join-Path $Repo 'build') --config Release -j 8 }
    } else {
        $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $VsPath  = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $VsPath) { throw 'Visual Studio with the C++ tools was not found' }
        # A .cmd file keeps the quoting simple: VsDevCmd only works inside cmd.exe.
        $Cmd = Join-Path ([IO.Path]::GetTempPath()) "conker_build_$PID.cmd"
        Set-Content -LiteralPath $Cmd -Encoding ascii -Value @(
            '@echo off'
            "call `"$VsPath\Common7\Tools\VsDevCmd.bat`" -arch=x64 -no_logo || exit /b 1"
            "cmake --build `"$Repo\build`" --config Release -j 8"
        )
        try { Invoke-Checked 'cmake --build' { cmd.exe /d /c $Cmd } } finally { Remove-Item -LiteralPath $Cmd }
    }

    Write-Host "Publishing conker-launcher.exe $Version"
    Remove-Item -LiteralPath $PubDir -Recurse -Force -ErrorAction SilentlyContinue
    Invoke-Checked 'dotnet publish' { dotnet publish (Join-Path $Repo 'launcher') -c Release -o $PubDir --nologo -v q }
}

# ── 2. Check what goes in ───────────────────────────────────────────────────
foreach ($Entry in $Files.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $Entry.Value -PathType Leaf)) {
        if ($Draft -and $Entry.Key -in $Docs) {
            Write-Warning "$($Entry.Key) is missing: left out (-Draft)"
            continue
        }
        throw "$($Entry.Key) is missing ($($Entry.Value))"
    }
}

foreach ($Exe in 'conker.exe', 'conker-launcher.exe') {
    $Info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Files[$Exe])
    $Have = '{0}.{1}.{2}' -f $Info.FileMajorPart, $Info.FileMinorPart, $Info.FileBuildPart
    if ($Have -ne $Version) {
        throw "$Exe is version $Have, VERSION says $Version (rebuild, or drop -SkipBuild)"
    }
}

# ── 3. Stage ─────────────────────────────────────────────────────────────────
Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue
foreach ($Entry in $Files.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $Entry.Value -PathType Leaf)) { continue }
    $Target = Join-Path $Stage $Entry.Key
    New-Item -ItemType Directory -Force (Split-Path -Parent $Target) | Out-Null
    Copy-Item -LiteralPath $Entry.Value -Destination $Target
}

# ── 4. Zip and check it ──────────────────────────────────────────────────────
Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($Stage, $Zip, [IO.Compression.CompressionLevel]::Optimal, $false)

$Expected = @($Files.Keys | Where-Object { Test-Path -LiteralPath (Join-Path $Stage $_) } | ForEach-Object { $_.Replace('\', '/') } | Sort-Object)
$Archive  = [IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $Inside = @($Archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object)
} finally {
    $Archive.Dispose()
}
$Diff = Compare-Object $Expected $Inside
if ($Diff) { throw "The zip doesn't hold exactly the release files:`n$($Diff | Out-String)" }
foreach ($Name in $Inside) {
    if ($Name -match '(^|/)(settings\.ini|game/|decensor/)|\.(pdb|xbe|iso|xiso|log)$') { throw "The zip holds $Name" }
}

# The .pdb stays here, never in the zip: a player's freeze report names our frames as
# conker.exe+offset (sample_profiler_dump_threads), and only this build's .pdb reads them.
$Symbols = Join-Path $OutDir 'symbols'
New-Item -ItemType Directory -Force $Symbols | Out-Null
Copy-Item -LiteralPath ($GameExe -replace '\.exe$', '.pdb') -Destination $Symbols -Force

$Hash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash
Write-Host ''
Write-Host "Release zip: $Zip"
Write-Host ("  {0:N1} MB, SHA-256 {1}" -f ((Get-Item -LiteralPath $Zip).Length / 1MB), $Hash)
foreach ($Name in $Inside) { Write-Host "  $Name" }
if ($Draft) { Write-Warning 'Draft zip (-Draft): not for a public release' }
