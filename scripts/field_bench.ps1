# Speed benchmark in one save's opening view, same two views every run: by
# default save 1, the first field ("Hungover"). Save 3 is the throne room.
#
# Start the game first with run.ps1 and RECOMP_PAD_LIVE=<PadFile> (plus
# RECOMP_SAMPLE_PROFILE=20 for [PROF] reports). Drive the menus by hand to the
# field; once Conker can be controlled, put the pad down and run this. It:
#   view A: holds still for -HoldSec (the spawn view, close wall on the left)
#   then appends -Turn to the pad file (default: right stick right = camera to
#   the waterway) and lets it settle
#   view B: holds still for -HoldSec
# and prints the [GPU] fps lines and texture counters seen during each view.
#
#   pwsh scripts\field_bench.ps1 -PadFile _work\pad_live.txt [-Log <run log>]
#
# -DriveToField does the menus too, so nobody has to be at the pad: run it
# right after starting the game. It waits for the log's "PAD: live input"
# line, then presses (after each wait): A (skip the intro), left to -Slot,
# A (select it), A (play it). The main menu opens on "Xbox Live"; each left
# moves one along: save 3, save 2, save 1, Chapters, Options, Xbox Live
# again (user, 2026-10-14). -MenuWaits gives the waits in seconds (the last
# one repeats if there are more presses). The
# field is taken as reached at the first [GPU] fps report below -FieldFps
# after the last press (menus run at ~260 fps, the field below 20), and
# the views start -FieldSettleSec later (the chapter's opening cutscene).

param(
    [Parameter(Mandatory)][string]$PadFile,
    [string]$Log,
    [int]$HoldSec = 40,
    [string]$Turn = 'rright:700',
    [int]$SettleSec = 5,
    [switch]$DriveToField,
    [ValidateRange(1, 3)][int]$Slot = 1,
    [int[]]$MenuWaits = @(13, 7, 7, 7),
    [double]$FieldFps = 30,
    [int]$FieldSettleSec = 30,
    [int]$TimeoutSec = 300
)

if (-not $Log) {
    if ($DriveToField) { Start-Sleep -Seconds 3 }   # let run.ps1 create it
    $Log = (Get-ChildItem "$PSScriptRoot\..\_work\logs" -Filter 'run_*.log' |
            Sort-Object LastWriteTime | Select-Object -Last 1).FullName
}
function LineCount { (Get-Content $Log -ReadCount 0).Count }

# Waits until $pattern shows up in the log after line $from; returns the
# match, or stops the script when -TimeoutSec runs out.
function WaitForLog($pattern, $from, $what) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $hit = (Get-Content $Log -ReadCount 0) | Select-Object -Skip $from |
               Select-String $pattern | Select-Object -First 1
        if ($hit) { return $hit }
        Start-Sleep -Seconds 1
    }
    throw "timed out after $TimeoutSec s waiting for $what"
}

if ($DriveToField) {
    $presses = @('a') + @('left') * (4 - $Slot) + @('a', 'a')
    "log: $Log"
    "save $Slot"
    WaitForLog 'PAD: live input' 0 'the pad file to be opened' | Out-Null
    for ($i = 0; $i -lt $presses.Count; $i++) {
        Start-Sleep -Seconds $MenuWaits[[Math]::Min($i, $MenuWaits.Count - 1)]
        Add-Content -Path $PadFile -Value $presses[$i]
        "pressed $($presses[$i])"
    }
    $from = LineCount
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ($true) {
        $hit = (Get-Content $Log -ReadCount 0) | Select-Object -Skip $from |
               Select-String '\[GPU\] ([0-9.]+) fps' |
               Where-Object { [double]$_.Matches[0].Groups[1].Value -lt $FieldFps } |
               Select-Object -First 1
        if ($hit) { break }
        if ((Get-Date) -gt $deadline) {
            throw "timed out after $TimeoutSec s waiting for the field (no fps report below $FieldFps)"
        }
        Start-Sleep -Seconds 1
    }
    "field reached ($($hit.Line.Trim())); settling $FieldSettleSec s"
    Start-Sleep -Seconds $FieldSettleSec
}

$a0 = LineCount
Start-Sleep -Seconds $HoldSec
$a1 = LineCount
Add-Content -Path $PadFile -Value $Turn
Start-Sleep -Seconds $SettleSec
$b0 = LineCount
Start-Sleep -Seconds $HoldSec
$b1 = LineCount

$lines = Get-Content $Log -ReadCount 0
function Summary($name, $from, $to) {
    $seg = $lines[$from..($to - 1)]
    $fps = @($seg | Select-String '\[GPU\] ([0-9.]+) fps' |
             ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    $tex = @($seg | Select-String '\[GPU\] d3d11: textures: (\d+) made, (\d+) uploads, (\d+) binds unchanged')
    $avg = if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { 0 }
    "{0}: log lines {1}-{2}, fps reports {3}, mean {4:N2} ({5})" -f $name, $from, $to,
        $fps.Count, $avg, (($fps | ForEach-Object { '{0:N1}' -f $_ }) -join ' ')
    if ($tex.Count -ge 2) {
        $f = $tex[0].Matches[0].Groups; $l = $tex[-1].Matches[0].Groups
        "  textures between first and last report: +{0} made, +{1} uploads, +{2} binds unchanged" -f `
            ([long]$l[1].Value - [long]$f[1].Value), ([long]$l[2].Value - [long]$f[2].Value),
            ([long]$l[3].Value - [long]$f[3].Value)
    }
}
"log: $Log"
Summary 'view A (spawn)' $a0 $a1
Summary 'view B (after turn)' $b0 $b1
