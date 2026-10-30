# Multiplayer character select, scrolled by script: the repro for the
# black screen / crash / rainbow textures there (conker-notes 2026-10-21).
#
# Start the game first (run.ps1 in the background) with
# RECOMP_PAD_LIVE=<abs path to -PadFile> and -SaveDir _work\bench_save_mp
# (a save folder whose profile was already picked once: the first visit to
# Multiplayer asks for it). This waits for "PAD: live input", then goes
# A (skip the movie), A (Multiplayer), A (Dumbots: first since Xbox Live is
# hidden, 2026-10-25), down x3 A
# (Chapter X), A, A (Second Assault), A (Tediz, or -Shc: up A), and presses
# left/right -Scrolls times (2 of 3 right; random gaps 150-550 ms, -Seed).
# The same seed gives the same presses, and so far the same addresses.
#
#   pwsh scripts\mp_select_repro.ps1 -PadFile _work\pad_live.txt [-ShotDir <dir>] [-Scrolls 150]
#   match start (load crash after the level movie): -Scrolls 0 -StartMatch
param(
    [Parameter(Mandatory)][string]$PadFile,
    [string]$Log,
    [string]$ShotDir,
    [int]$Scrolls = 150,
    [int]$Seed = 1,
    [switch]$Shc,
    [switch]$StartMatch,       # then pick, start the match and skip its movie (use -Scrolls 0)
    [int]$ClassRight = 2,      # rights before picking (left/right change class): 0 and 1 land on X'd classes on Chapter X
    [double]$WaitScale = 1.0   # stretch the menu waits (slower loads, e.g. RECOMP_IO_LATENCY_MS)
)
$repo = Split-Path -Parent $PSScriptRoot
$started = (Get-Date).AddSeconds(-15)
Start-Sleep -Seconds 3
if (-not $Log) {
    # The newest log created since just before this started, so a run still
    # shutting down is not taken for the new one.
    $Log = (Get-ChildItem "$repo\_work\logs" -Filter 'run_*.log' | Where-Object { $_.Name -notmatch 'kernel' -and $_.CreationTime -ge $started } |
            Sort-Object CreationTime | Select-Object -Last 1).FullName
    if (-not $Log) { throw 'no new run log; start run.ps1 first' }
}
"log: $Log"
if ($ShotDir) { New-Item -ItemType Directory -Force $ShotDir | Out-Null }
$deadline = (Get-Date).AddSeconds(90)
while (-not (Select-String -Path $Log -Pattern 'PAD: live input' -Quiet)) {
    if ((Get-Date) -gt $deadline) { throw 'no PAD: live input in the log' }
    Start-Sleep -Milliseconds 500
}
function Shot($name) {
    if ($ShotDir) {
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$repo\scripts\gamewin.ps1" -Shot "$ShotDir\$name.png" | Out-Null
    }
}
function Press($b, $wait) {
    Add-Content -Path $PadFile -Value $b
    Start-Sleep -Milliseconds ([int]($wait * $script:WaitScale))
}
$steps = @(@('a',12000,'01_intro'), @('a',6000,'02_multiplayer'), @('a',5000,'03_bots'),
           @('down',1200,''), @('down',1200,''), @('down',1200,''), @('a',5000,'04_chapter'), @('a',5000,'05_worm'),
           @('a',10000,'06_team'))
Start-Sleep -Seconds 10
Shot '00_start'
foreach ($s in $steps) { Press $s[0] $s[1]; if ($s[2]) { Shot $s[2] } }
if ($Shc) { Press 'up' 1200 }
Press 'a' 8000
Shot '07_chars'
$rng = [System.Random]::new($Seed)
for ($i = 0; $i -lt $Scrolls; $i++) {
    $b = if ($rng.Next(3) -eq 0) { 'left' } else { 'right' }
    Add-Content -Path $PadFile -Value "${b}:100"           # scrolling stays fast
    Start-Sleep -Milliseconds (150 + $rng.Next(400))
    if ($ShotDir -and ($i % 25) -eq 24) { Shot ("08_scroll_{0:D3}" -f $i) }
}
'done scrolling'
if ($StartMatch) {
    # A (pick the character), A (start the match), A (skip the level's movie), then
    # watch the match load: the crash after the movie (conker-notes 2026-10-21).
    for ($i = 0; $i -lt $ClassRight; $i++) { Press 'right' 1500 }
    Press 'a' 5000; Shot '09_picked'
    Press 'a' 15000; Shot '10_movie'
    Add-Content -Path $PadFile -Value 'a'
    # A screenshot every 2 s through the load and the round start.
    for ($i = 0; $i -lt 45; $i++) { Start-Sleep -Seconds 2; Shot ("11_match_{0:D2}" -f $i) }
    'done starting the match'
}
