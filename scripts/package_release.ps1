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
       logo and the cheat words (fire_imps.journal) in textures\, the menus' translations
       (lang\*.txt, checked by scripts\check_lang.py first), LICENSE.txt,
       THIRD_PARTY_NOTICES.txt, README.txt, DEBUG_MODE.txt (release\DEBUG_MODE.md),
       TEXTURE_PACKS.txt (release\TEXTURE_PACKS.md), add-to-steam.sh (release\, LF line
       endings checked).
    4. Zips it, marks add-to-steam.sh executable for Linux (mode 0755 in its zip entry), and
       checks the zip holds exactly that list: never settings.ini or anything else from a
       save folder (SPEC 9.6), never game\ or decensor\ (SPEC 9.1).

    The launcher's updater replaces the files this zip holds and keeps everything else
    in the player's folder, so a file added here reaches players on their next update.

.PARAMETER SkipBuild
    Package what build\Release and the last publish hold, without building.

.PARAMETER Draft
    Allows LICENSE.txt, THIRD_PARTY_NOTICES.txt, README.txt, DEBUG_MODE.txt and TEXTURE_PACKS.txt to be missing (the private
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
    # The texture pack guide (to-do 238, SPEC 9.2 "Texture pack kit"), Markdown read as plain text too.
    'TEXTURE_PACKS.txt'                            = Join-Path $Repo 'release\TEXTURE_PACKS.md'
    # Add to Steam on Linux and the Steam Deck (to-do 285, SPEC 9.7): executable in the zip (step 4).
    'add-to-steam.sh'                              = Join-Path $Repo 'release\add-to-steam.sh'
}
$Docs = 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'README.txt', 'DEBUG_MODE.txt', 'TEXTURE_PACKS.txt'
# The menus' translations (to-do 229, SPEC 10.1 "Menu language"): our own text, from the repo.
foreach ($Lang in Get-ChildItem -LiteralPath (Join-Path $Repo 'lang') -Filter '*.txt' | Sort-Object Name) {
    $Files["lang\$($Lang.Name)"] = $Lang.FullName
}

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

# bash stops at the first line with a Windows line ending ($'\r': command not found).
if ([IO.File]::ReadAllText($Files['add-to-steam.sh']).Contains("`r")) {
    throw 'release\add-to-steam.sh has Windows line endings (CR LF): bash on Linux would fail (.gitattributes keeps *.sh LF)'
}

# A translation that drops or reorders a %s would print garbage or crash: refused here.
$env:PYTHONIOENCODING = 'utf-8'
$LangReport = & (Join-Path $Repo '.venv\Scripts\python.exe') (Join-Path $Repo 'scripts\check_lang.py')
$LangCode = $LASTEXITCODE
$LangReport | Select-String -Pattern 'translated|PRINTF|LINES' | ForEach-Object { Write-Host "  $_" }
if ($LangCode -ne 0) { throw 'scripts\check_lang.py: a translation breaks a %s or a line break (see above)' }
# Every menu text ships translated (user 2026-11-25, CLAUDE.md "Conventions"): a missing line
# would show English in that language's menu. -Draft lets it pass.
$Missing = $LangReport | Select-String -Pattern '(\d+) missing' | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 0 }
if ($Missing -and -not $Draft) {
    throw 'lang\*.txt: menu texts without a translation (see "missing" above): fill them in every language first'
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
# A zip made on Windows marks every file "made on Windows", so Linux unpacks add-to-steam.sh
# without its executable bit (a double-click on the Deck would open it in a text editor).
# Its central directory entry is marked "made on Unix" with mode 0755 (rwxr-xr-x), which
# Info-ZIP's unzip, libarchive and Ark honour. Nothing else in the zip changes.
function Set-ZipEntryExecutable([string]$ZipPath, [string]$EntryName) {
    $Bytes = [IO.File]::ReadAllBytes($ZipPath)
    $Eocd = -1      # end of central directory record, searched from the end (a comment can follow it)
    for ($i = $Bytes.Length - 22; $i -ge [Math]::Max(0, $Bytes.Length - 22 - 65535); $i--) {
        if ([BitConverter]::ToUInt32($Bytes, $i) -eq 0x06054b50) { $Eocd = $i; break }
    }
    if ($Eocd -lt 0) { throw "$ZipPath has no end of central directory record" }
    $Count = [BitConverter]::ToUInt16($Bytes, $Eocd + 10)
    $Pos   = [int][BitConverter]::ToUInt32($Bytes, $Eocd + 16)
    $Found = 0
    for ($n = 0; $n -lt $Count; $n++) {
        if ([BitConverter]::ToUInt32($Bytes, $Pos) -ne 0x02014b50) { throw "$ZipPath`: no central directory entry at byte $Pos" }
        $NameLen = [BitConverter]::ToUInt16($Bytes, $Pos + 28)
        $Skip    = [BitConverter]::ToUInt16($Bytes, $Pos + 30) + [BitConverter]::ToUInt16($Bytes, $Pos + 32)
        if ([Text.Encoding]::UTF8.GetString($Bytes, $Pos + 46, $NameLen) -eq $EntryName) {
            $Bytes[$Pos + 5] = 3                    # "version made by" high byte: host 3 = Unix
            $Bytes[$Pos + 38] = 0; $Bytes[$Pos + 39] = 0    # external attributes: no DOS flags,
            $Bytes[$Pos + 40] = 0xED; $Bytes[$Pos + 41] = 0x81   # high 16 bits = 0o100755
            $Found++
        }
        $Pos += 46 + $NameLen + $Skip
    }
    if ($Found -ne 1) { throw "$ZipPath holds $EntryName $Found times, not once" }
    [IO.File]::WriteAllBytes($ZipPath, $Bytes)
}

Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($Stage, $Zip, [IO.Compression.CompressionLevel]::Optimal, $false)
Set-ZipEntryExecutable $Zip 'add-to-steam.sh'

$Expected = @($Files.Keys | Where-Object { Test-Path -LiteralPath (Join-Path $Stage $_) } | ForEach-Object { $_.Replace('\', '/') } | Sort-Object)
$Archive  = [IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $Inside = @($Archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object)
    $Script = $Archive.GetEntry('add-to-steam.sh')
    if (-not $Script -or (($Script.ExternalAttributes -shr 16) -band 0xFFFF) -ne 0x81ED) {
        throw "add-to-steam.sh isn't marked executable in the zip"
    }
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
