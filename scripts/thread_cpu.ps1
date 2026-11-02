# Per-thread CPU % of a running conker.exe, one block per window (CPU load work,
# conker-notes 2026-11-01). Start it, then the game; it waits up to 2 min for the process:
#   Start-Process pwsh -ArgumentList '-NoProfile','-File','scripts\thread_cpu.ps1','-Out','_work\cpu.txt' -WindowStyle Hidden
# Thread ids match [PROF] lines (RECOMP_SAMPLE_PROFILE=20) for names.
param([string]$Out, [int]$Every = 10, [int]$MaxSec = 400)
$t0 = Get-Date
while (-not ($p = Get-Process conker -ErrorAction SilentlyContinue)) {
    if (((Get-Date) - $t0).TotalSeconds -gt 120) { "no conker"; exit 1 }
    Start-Sleep -Milliseconds 500
}
$start = Get-Date
$prev = @{}; $prevT = Get-Date; $prevProc = $p.TotalProcessorTime
"cores: $([Environment]::ProcessorCount)" | Out-File $Out
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $MaxSec) {
    Start-Sleep -Seconds $Every
    try { $p.Refresh() } catch { break }
    if ($p.HasExited) { break }
    $now = Get-Date; $dt = ($now - $prevT).TotalSeconds
    $rows = foreach ($t in $p.Threads) {
        try { $cpu = $t.TotalProcessorTime.TotalSeconds; $usr = $t.UserProcessorTime.TotalSeconds } catch { continue }
        $id = $t.Id
        if ($prev.ContainsKey($id)) {
            $d = $cpu - $prev[$id][0]; $du = $usr - $prev[$id][1]
            [pscustomobject]@{ tid = $id; pct = [math]::Round(100 * $d / $dt, 1); user = [math]::Round(100 * $du / $dt, 1); start = ('{0:X}' -f [int64]$t.StartAddress) }
        }
        $prev[$id] = @($cpu, $usr)
    }
    $tot = ($p.TotalProcessorTime - $prevProc).TotalSeconds / $dt * 100
    $prevProc = $p.TotalProcessorTime; $prevT = $now
    "--- t=$([int]($now - $start).TotalSeconds)s total $([math]::Round($tot,0))% of one core" | Out-File $Out -Append
    $rows | Where-Object pct -ge 1 | Sort-Object pct -Descending | Format-Table -AutoSize | Out-String | Out-File $Out -Append
}
