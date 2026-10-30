param([string]$Action = "list", [string]$Name = "", [string]$Value = "", [string]$Process = "conker-launcher")
# Drives the launcher through UI Automation (no mouse, nothing in front of the user):
#   -Action list | tab -Name SETTINGS | toggle -Name "Full screen" | invoke -Name "Reset to defaults" | radio -Name Modern
#   | select -Name "Frame rate" -Value "30 fps (original)" | range -Name Gamma -Value 1.2
#   | key -Value 4B (a key press posted to the window, hex virtual-key code: the Controls tab's "Press a key")
# Windows PowerShell: powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\launcher_uia.ps1 ...
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$p = Get-Process $Process | Select-Object -First 1
if ($Action -eq "key") {
    Add-Type -Namespace U -Name W -MemberDefinition @"
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
"@
    $vk = [Convert]::ToUInt32($Value, 16)
    $scan = [U.W]::MapVirtualKey($vk, 0)
    [U.W]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr]$vk, [IntPtr](1 -bor ($scan -shl 16))) | Out-Null
    [U.W]::PostMessage($p.MainWindowHandle, 0x101, [IntPtr]$vk, [IntPtr](1 -bor ($scan -shl 16) -bor (3 -shl 30))) | Out-Null
    return
}
$root = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
$all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
function Find($n, $t = "") {
    foreach ($e in $all) {
        if ($e.Current.Name -eq $n -and (-not $t -or $e.Current.ControlType.ProgrammaticName -eq "ControlType.$t")) { return $e }
    }
    throw "not found: $n"
}
switch ($Action) {
    "list" {
        foreach ($e in $all) {
            $c = $e.Current
            if ($c.Name) { "{0} | {1} | enabled={2}" -f $c.ControlType.ProgrammaticName, $c.Name, $c.IsEnabled }
        }
    }
    "tab" { (Find $Name "TabItem").GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
    "radio" { (Find $Name "RadioButton").GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
    "toggle" { (Find $Name "CheckBox").GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle() }
    "invoke" { (Find $Name "Button").GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
    "select" {
        $combo = Find $Name "ComboBox"
        $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
        Start-Sleep -Milliseconds 400
        $items = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)))
        $hit = $null
        foreach ($i in $items) { if ($i.Current.Name -eq $Value -and $i.Current.ProcessId -eq $p.Id) { $hit = $i } }
        if (-not $hit) { "items: " + (($items | Where-Object { $_.Current.ProcessId -eq $p.Id } | ForEach-Object { $_.Current.Name }) -join ' / '); throw "no item $Value" }
        $hit.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
        Start-Sleep -Milliseconds 200
        try { $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse() } catch {}
    }
    "range" { (Find $Name "Slider").GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern).SetValue([double]$Value) }
}
