<#
conker.exe --pad-feed by hand (to-do 121, SPEC 9.2). Starts the helper as the launcher
does (hidden, input and output redirected), prints every line with the seconds since the
start and the buttons it means, and its stderr ([INPUT] lines) marked "err". At the end
it prints the helper's CPU use, closes its input and checks that it quits by itself.

  pwsh scripts\pad_feed_test.ps1 [-Seconds 60] [-Exe build\Release\conker.exe] [-Log <file>]
  pwsh scripts\pad_feed_test.ps1 -Replay <log>    # a saved -Log with the button names again

Press buttons, move sticks, plug / unplug controllers while it runs. Ctrl+C ends it early.
#>
param(
    [int]$Seconds = 60,
    [string]$Exe = "build\Release\conker.exe",
    [string]$Log,
    [string]$Replay
)

# Bit, then its name: a list, not a hashtable (an ordered hashtable indexed with a number
# gives the entry at that position, not the key).
$names = @(
    @(0x0001, 'Up'), @(0x0002, 'Down'), @(0x0004, 'Left'), @(0x0008, 'Right'),
    @(0x0010, 'Start'), @(0x0020, 'Back'), @(0x0040, 'L3'), @(0x0080, 'R3'),
    @(0x0100, 'LB'), @(0x0200, 'RB'), @(0x1000, 'A'), @(0x2000, 'B'), @(0x4000, 'X'), @(0x8000, 'Y'),
    @(0x10000, 'LT'), @(0x20000, 'RT'),
    @(0x40000, 'LS-up'), @(0x80000, 'LS-down'), @(0x100000, 'LS-left'), @(0x200000, 'LS-right'),
    @(0x400000, 'RS-up'), @(0x800000, 'RS-down'), @(0x1000000, 'RS-left'), @(0x2000000, 'RS-right')
)

function Describe([string]$line) {
    if ($line -match '^pad (\d) (\w+) ([0-9A-Fa-f]+)$') {
        $v = [Convert]::ToUInt32($Matches[3], 16)
        $held = @(); $known = 0
        foreach ($n in $names) {
            $known = $known -bor $n[0]
            if ($v -band $n[0]) { $held += $n[1] }
        }
        if ($v -band -bnot $known) { $held += ('?0x{0:X}' -f ($v -band -bnot $known)) }
        return '   = ' + $(if ($held.Count) { $held -join ' + ' } else { '(nothing held)' })
    }
    return ''
}

# -Replay <log>: name the buttons in a saved log again (the time column is kept).
if ($Replay) {
    Get-Content $Replay | ForEach-Object {
        if ($_ -match '^\s*([\d.]+) (pad \d \w+ [0-9A-Fa-f]+)') {
            '{0,7} {1}{2}' -f $Matches[1], $Matches[2], (Describe $Matches[2])
        } else { $_ }
    }
    return
}

$exePath = (Resolve-Path $Exe).Path
$psi = [System.Diagnostics.ProcessStartInfo]::new($exePath, '--pad-feed')
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
"helper pid $($p.Id), $Seconds s: press buttons now"
$out = $p.StandardOutput.ReadLineAsync()
$err = $p.StandardError.ReadLineAsync()
$lines = [System.Collections.Generic.List[string]]::new()

function Show([string]$s) {
    $t = '{0,7:0.000} {1}' -f $sw.Elapsed.TotalSeconds, $s
    $t
    $lines.Add($t)
}

try {
    while ($sw.Elapsed.TotalSeconds -lt $Seconds -and !$p.HasExited) {
        $i = [System.Threading.Tasks.Task]::WaitAny(@($out, $err), 100)
        if ($i -eq 0) {
            if ($null -eq $out.Result) { break }
            Show ($out.Result + (Describe $out.Result))
            $out = $p.StandardOutput.ReadLineAsync()
        } elseif ($i -eq 1) {
            if ($null -ne $err.Result) { Show ('err ' + $err.Result) }
            $err = if ($null -ne $err.Result) { $p.StandardError.ReadLineAsync() } else { [System.Threading.Tasks.Task]::Delay(-1) }
        }
    }
} finally {
    if (!$p.HasExited) {
        $p.Refresh()
        $cpu = $p.TotalProcessorTime.TotalMilliseconds
        $secs = $sw.Elapsed.TotalSeconds
        $pct = 100 * $cpu / $sw.Elapsed.TotalMilliseconds
        Show ('helper CPU: {0:0} ms in {1:0.0} s = {2:0.00} % of one core' -f $cpu, $secs, $pct)
        $t0 = $sw.ElapsedMilliseconds
        $p.StandardInput.Close()
        if ($p.WaitForExit(3000)) {
            Show ("input closed: helper quit after $($sw.ElapsedMilliseconds - $t0) ms, exit code $($p.ExitCode)")
        } else {
            Show 'input closed: helper STILL RUNNING after 3 s (killed)'
            $p.Kill()
        }
    } else {
        Show "helper ended by itself, exit code $($p.ExitCode)"
    }
    if ($Log) { $lines | Set-Content -Path $Log -Encoding utf8 }
}
