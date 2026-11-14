<#
.SYNOPSIS
  Runs debug-console commands in a running conker.exe (--debug) through its
  command pipe (to-do 174, SPEC 10.3) and prints the replies.

.EXAMPLE
  powershell -File scripts\conker_cmd.ps1 pos
  powershell -File scripts\conker_cmd.ps1 "warp cp 12" waitload pos "shot after_warp"
  powershell -File scripts\conker_cmd.ps1 -Pipe conker-test "exec D:\routes\field.txt"

.NOTES
  Each argument is one command line, run in order; the script waits for each
  reply (wait, waitload, exec and shot answer when they are done). Exit code
  0 = every command answered "ok", 1 = some printed an error, 2 = no game
  (no pipe) or no answer in time. -Pipe: the name the game took (its log's
  [PIPE] line; default conker-console, RECOMP_CMD_PIPE sets another).
  Works in Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Pipe = 'conker-console',
    [int]$TimeoutSec = 600,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Commands
)

if (-not $Commands -or $Commands.Count -eq 0) {
    Write-Host 'Usage: conker_cmd.ps1 [-Pipe <name>] [-TimeoutSec <s>] <command> [<command> ...]'
    exit 2
}

$client = New-Object System.IO.Pipes.NamedPipeClientStream('.', $Pipe, [System.IO.Pipes.PipeDirection]::InOut)
try {
    $client.Connect(3000)
} catch {
    Write-Host "No game on \\.\pipe\$Pipe (is conker.exe running with --debug?)"
    exit 2
}
$enc = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($client, $enc)
$writer.NewLine = "`n"
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($client, $enc)
$code = 0
try {
    foreach ($cmd in $Commands) {
        $writer.WriteLine($cmd)
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ($true) {
            $task = $reader.ReadLineAsync()
            while (-not $task.Wait(200)) {
                if ((Get-Date) -gt $deadline) {
                    Write-Host "No answer to '$cmd' in $TimeoutSec s"
                    exit 2
                }
            }
            $line = $task.Result
            if ($null -eq $line) { Write-Host 'The game closed the pipe'; exit 2 }
            if ($line -eq '= ok') { break }
            if ($line -eq '= error') { $code = 1; break }
            if ($line.StartsWith(' =')) { $line = $line.Substring(1) }
            Write-Output $line
        }
    }
} finally {
    $client.Dispose()
}
exit $code
