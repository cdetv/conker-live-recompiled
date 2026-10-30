# Speed benchmark at a save's spawn point: starts the game (run.ps1), loads
# save -Slot through the menus with a live pad file, stands still, and
# averages the [GPU] fps reports between -StartSec and -StartSec + -HoldSec
# after the last menu press (the level load is inside -StartSec). Made for
# the Uga Buga bomb run in save 2 (nightclub spawn, 1:30 timer, no enemies
# at the start): the slowest spot so far, ~23 fps at 1080p (2026-10-16).
# Keep the window short: standing still, Conker blows up when the timer
# runs out and the game-over cutscene (30 fps, then 0 while it reloads)
# lands ~40 s into a longer window.
# Uses a copy of the saves (-SaveDir), never the real save folder.
# The game saves as it goes (lives, checkpoints), so a -SaveDir changes
# with every run; -SaveMaster <dir> copies that untouched folder over
# -SaveDir first (sparse, as the partition images are), so every run starts
# from the same save. -SaveDir must then be inside _work.
#
#   pwsh scripts\slot_bench.ps1 -SaveDir _work\bench_save_20261016 [-Exe <exe>] [-Env A=1,B=2]
#   pwsh scripts\slot_bench.ps1 -SaveMaster _work\bench_save_mansion_master -SaveDir _work\bench_save_mansion
#
# The main menu opens on "Xbox Live"; each left moves one along: save 3,
# save 2, save 1 (see field_bench.ps1). Prints mean, min and max fps and
# the [HITCH] 10 s summaries seen in the window (when RECOMP_HITCH_MS is set).
# -ShotEvery N: a screenshot of the game every N s after the last press
# (_work\bench_shots\<log name>_<s>.png), to see what each report covered.
# -Then "<s>=<pad line>",...: pad file lines at those seconds after the last
# press, e.g. "22=lup:1000" (left stick forward for 1 s); "key:<hex>:<ms>"
# holds that key (virtual-key code, gamewin.ps1) instead, e.g. "34=key:20:1500"
# (Space for 1.5 s; the keyboard-and-mouse bindings).
# -Touch "<s>=<file>",...: create that file at those seconds after the last
# press, e.g. "76=_work	race_now" with RECOMP_FRAME_TRACE (a frame trace at a
# known moment of a cutscene).
# -Presses a,right,...: these menu presses instead of loading -Slot (one per
# -MenuWaits entry), e.g. the chapter select (see conker-notes 2026-10-26).

param(
    [Parameter(Mandatory)][string]$SaveDir,
    [string]$SaveMaster = '',
    [string]$Exe = 'build\Release\conker.exe',
    [string]$Env = '',
    [ValidateRange(1, 3)][int]$Slot = 2,
    [int[]]$MenuWaits = @(13, 7, 7, 7, 7),
    [int]$StartSec = 30,
    [int]$HoldSec = 36,
    [int]$ShotEvery = 0,
    [string[]]$Then = @(),
    [string[]]$Touch = @(),
    [string[]]$Presses = @()
)

$Repo = Resolve-Path "$PSScriptRoot\.."
if ($SaveMaster) {
    $work = (Resolve-Path "$Repo\_work").Path
    $full = if ([IO.Path]::IsPathRooted($SaveDir)) { [IO.Path]::GetFullPath($SaveDir) }
            else { [IO.Path]::GetFullPath((Join-Path $Repo $SaveDir)) }
    if (-not $full.StartsWith($work + '\')) { throw "-SaveDir must be inside _work with -SaveMaster: $full" }
    if (-not (Test-Path (Join-Path $SaveMaster 'Partition1.img'))) { throw "no save in -SaveMaster $SaveMaster" }
    if (Test-Path $full) { Remove-Item $full -Recurse -Force -Confirm:$false }
    & 'C:\Program Files\Git\usr\bin\cp.exe' -r --sparse=always ((Resolve-Path $SaveMaster).Path -replace '\\', '/') ($full -replace '\\', '/')
    if ($LASTEXITCODE) { throw "copying $SaveMaster failed" }
    "save: fresh copy of $SaveMaster"
}
$Pad = Join-Path $Repo '_work\pad_live.txt'
Set-Content -Path $Pad -Value $null
$envs = @("RECOMP_PB_REPORT_MS=2000", "RECOMP_PAD_LIVE=$($Pad -replace '\\', '/')")
if ($Env) { $envs = @($Env -split ',') + $envs }
$timeout = ($MenuWaits | Measure-Object -Sum).Sum + $StartSec + $HoldSec + 25

$before = Get-ChildItem "$Repo\_work\logs" -Filter 'run_*.log' | ForEach-Object FullName
Start-Process pwsh -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-File', "$Repo\scripts\run.ps1", '-TimeoutSec', $timeout,
    '-Exe', $Exe, '-SaveDir', $SaveDir, '-Env', ($envs -join ','))
$Log = $null
while (-not $Log) {
    Start-Sleep -Seconds 1
    $Log = Get-ChildItem "$Repo\_work\logs" -Filter 'run_*.log' |
           Where-Object { $before -notcontains $_.FullName } |
           Select-Object -First 1 -ExpandProperty FullName
}
"log: $Log"
# Read with FileShare.ReadWrite: the game is writing the log meanwhile, and
# Get-Content once came back with 2 lines mid-run.
function LineCount {
    $fs = [IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object IO.StreamReader($fs)
    $n = 0
    while ($null -ne $sr.ReadLine()) { $n++ }
    $sr.Close()
    $n
}
while (-not (Select-String -Path $Log -Pattern 'PAD: live input' -Quiet)) { Start-Sleep -Seconds 1 }

$presses = if ($Presses) { $Presses } else { @('a') + @('left') * (4 - $Slot) + @('a', 'a') }
for ($i = 0; $i -lt $presses.Count; $i++) {
    Start-Sleep -Seconds $MenuWaits[[Math]::Min($i, $MenuWaits.Count - 1)]
    Add-Content -Path $Pad -Value $presses[$i]
}
# After the last press: -Then presses at their times, screenshots, and the
# window's first line at -StartSec, in time order.
$events = @(@{ t = [double]$StartSec; k = 'start' })
foreach ($e in $Then) {
    $sec, $line = $e -split '=', 2
    $events += @{ t = [double]$sec; k = 'pad'; v = $line }
}
foreach ($e in $Touch) {
    $sec, $file = $e -split '=', 2
    $events += @{ t = [double]$sec; k = 'touch'; v = $file }
}
if ($ShotEvery -gt 0) {
    $shots = Join-Path $Repo '_work\bench_shots'
    New-Item -ItemType Directory -Force $shots | Out-Null
    $tag = [IO.Path]::GetFileNameWithoutExtension($Log)
    for ($s = 0; $s -lt $StartSec + $HoldSec; $s += $ShotEvery) {
        $events += @{ t = [double]$s; k = 'shot' }
    }
}
$events += @{ t = [double]($StartSec + $HoldSec); k = 'end' }
$t0 = Get-Date
foreach ($e in ($events | Sort-Object { $_.t }, { $_.k -ne 'pad' })) {
    $wait = $e.t - ((Get-Date) - $t0).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    switch ($e.k) {
        'start' { $a0 = LineCount }
        'pad'   {
            if ($e.v -like 'key:*') {
                $null, $vk, $ms = $e.v -split ':'
                "  {0,5:N1} s: key {1} for {2} ms (line {3})" -f ((Get-Date) - $t0).TotalSeconds, $vk, $ms, (LineCount)
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$Repo\scripts\gamewin.ps1" -Keys $vk -HoldMs $ms -DelayMs 0 | Out-Null
            } else {
                Add-Content -Path $Pad -Value $e.v; "  {0,5:N1} s: pad {1}" -f ((Get-Date) - $t0).TotalSeconds, $e.v
            }
        }
        'touch' { New-Item -ItemType File -Force $e.v | Out-Null; "  {0,5:N1} s: touch {1} (line {2})" -f ((Get-Date) - $t0).TotalSeconds, $e.v, (LineCount) }
        'shot'  {
            $s = ((Get-Date) - $t0).TotalSeconds
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$Repo\scripts\gamewin.ps1" -Shot (Join-Path $shots ("{0}_{1:000}s_line{2}.png" -f $tag, [int]$s, (LineCount))) | Out-Null
        }
    }
}
$a1 = LineCount
while (-not (Select-String -Path $Log -Pattern '^# result' -Quiet)) { Start-Sleep -Seconds 2 }

$seg = (Get-Content $Log -ReadCount 0)[$a0..($a1 - 1)]
$fps = @($seg | Select-String '\[GPU\] ([0-9.]+) fps' |
         ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
if (-not $fps.Count) { "no fps reports in the window (lines $a0-$a1)"; exit 1 }
$m = $fps | Measure-Object -Average -Minimum -Maximum
$sorted = $fps | Sort-Object
$median = $sorted[[int][Math]::Floor(($sorted.Count - 1) / 2)]
"slot $Slot, $($fps.Count) fps reports (lines $a0-$a1): mean {0:N2}, median {1:N2}, min {2:N2}, max {3:N2}" -f $m.Average, $median, $m.Minimum, $m.Maximum
"  " + (($fps | ForEach-Object { '{0:N1}' -f $_ }) -join ' ')
$seg | Select-String '^\[HITCH\] [0-9.]+ s:' | ForEach-Object { '  ' + $_.Line }
