<#
.SYNOPSIS
    Send keys to the running game's window and/or screenshot it, without
    anyone at the keyboard (tests of the F2 menu and other window keys).
.DESCRIPTION
    Keys are posted as WM_KEYDOWN/WM_KEYUP to conker's main window,
    so they reach the window procedure (and the F2 menu's hook) even when the
    window is not in front. The screenshot uses PrintWindow with full content,
    which captures the D3D11 swap chain too.

    Run with Windows PowerShell 5.1, not pwsh 7 (pwsh lacks System.Drawing):
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\gamewin.ps1 -Keys 71 -DelayMs 1500 -Shot _work\shot.png
.PARAMETER Keys
    Comma list of virtual-key codes in hex: 71 = F2, 1B = Esc, 0D = Enter,
    20 = Space, 26/28 = Up/Down, 25/27 = Left/Right.
.PARAMETER Click
    Left click after the keys, at "x,y" in the game window's client area
    (pixels): e.g. outside the F2 menu to give the pad back to the game.
.PARAMETER Drag
    Left-button drag after the keys and click, "x1,y1,x2,y2" in client
    pixels, in 10 steps: e.g. the F2 menu's title bar (move) or its
    bottom-right corner (resize).
.PARAMETER Shot
    PNG to write after the keys and click (client area of the game window).
.PARAMETER DelayMs
    Wait after each key, so the game can draw the result.
.PARAMETER HoldMs
    How long each key is held (default 60 ms, a tap).
.PARAMETER Process
    Process whose main window to use (default conker). xemu's
    OpenGL window screenshots black with PrintWindow.
#>
param(
    [string]$Keys = "",
    [string]$Click = "",
    [string]$Drag = "",
    [string]$Shot = "",
    [int]$DelayMs = 150,
    [int]$HoldMs = 60,
    [string]$Process = "conker"   # e.g. xemu for a screenshot of the emulator
)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
public static class GW {
    [DllImport("user32.dll")] public static extern bool PostMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static void Shot(IntPtr h, string path) {
        RECT r; GetClientRect(h, out r);
        using (var bmp = new Bitmap(Math.Max(1, r.R), Math.Max(1, r.B))) {
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr dc = g.GetHdc();
                PrintWindow(h, dc, 3);   // client area only | full content
                g.ReleaseHdc(dc);
            }
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
"@
# The process's main window. FindWindow by class does not work from here:
# PowerShell passes $null for a string as "", not NULL.
$p = Get-Process $Process -ErrorAction SilentlyContinue | Select-Object -First 1
$h = if ($p) { $p.MainWindowHandle } else { [IntPtr]::Zero }
if ($h -eq [IntPtr]::Zero) { Write-Output "no game window"; exit 1 }
foreach ($k in ($Keys -split ',' | Where-Object { $_ })) {
    $vk = [Convert]::ToInt32($k, 16)
    [void][GW]::PostMessageA($h, 0x100, [IntPtr]$vk, [IntPtr]1)            # WM_KEYDOWN
    Start-Sleep -Milliseconds $HoldMs
    [void][GW]::PostMessageA($h, 0x101, [IntPtr]$vk, [IntPtr]0xC0000001)   # WM_KEYUP
    Start-Sleep -Milliseconds $DelayMs
}
if ($Click) {
    $x, $y = $Click -split ',' | ForEach-Object { [int]$_ }
    $l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [void][GW]::PostMessageA($h, 0x200, [IntPtr]0, $l)   # WM_MOUSEMOVE
    [void][GW]::PostMessageA($h, 0x201, [IntPtr]1, $l)   # WM_LBUTTONDOWN (MK_LBUTTON)
    Start-Sleep -Milliseconds 60
    [void][GW]::PostMessageA($h, 0x202, [IntPtr]0, $l)   # WM_LBUTTONUP
    Start-Sleep -Milliseconds $DelayMs
}
if ($Drag) {
    $x1, $y1, $x2, $y2 = $Drag -split ',' | ForEach-Object { [int]$_ }
    $lp = { param($x, $y) [IntPtr](([int]$y -shl 16) -bor ([int]$x -band 0xFFFF)) }
    [void][GW]::PostMessageA($h, 0x200, [IntPtr]0, (& $lp $x1 $y1))
    Start-Sleep -Milliseconds 100
    [void][GW]::PostMessageA($h, 0x201, [IntPtr]1, (& $lp $x1 $y1))
    for ($i = 1; $i -le 10; $i++) {
        Start-Sleep -Milliseconds 70
        $x = $x1 + ($x2 - $x1) * $i / 10; $y = $y1 + ($y2 - $y1) * $i / 10
        [void][GW]::PostMessageA($h, 0x200, [IntPtr]1, (& $lp $x $y))
    }
    Start-Sleep -Milliseconds 100
    [void][GW]::PostMessageA($h, 0x202, [IntPtr]0, (& $lp $x2 $y2))
    Start-Sleep -Milliseconds $DelayMs
}
if ($Shot) { [GW]::Shot($h, $Shot); Write-Output "shot $Shot" }
