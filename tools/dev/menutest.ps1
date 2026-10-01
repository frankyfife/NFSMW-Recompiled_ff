param([string]$Name, [string[]]$Extra, [int]$Measure = 25)
# Starts the game, skips the intro movies with Enter, then logs frame pacing in the menu.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$b = "D:\NFSMW\NFSMW-Recompiled_ff\build"
$log = Join-Path $env:TEMP ("nfsmw_menu_" + $Name + ".log")
if (Test-Path $log) { Remove-Item -LiteralPath $log }
$a = @('--game_data_root', "$b\game_root", '--gpu_plugin', 'xenos', '--fullscreen=false',
       '--resolution', '720p', '--max_fps', '0', '--log_guest_fps=true', '--log_file', $log) + $Extra
$p = Start-Process "$b\nfsmw.exe" -ArgumentList $a -WorkingDirectory $b -PassThru
Start-Sleep -Seconds 8
for ($i = 0; $i -lt 10; $i++) {
  $p.Refresh()
  [K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
  Start-Sleep -Milliseconds 150
  [K]::keybd_event(0x0D, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
  [K]::keybd_event(0x0D, 0, 2, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 1800
}
Start-Sleep -Seconds $Measure
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2
"== $Name"
Get-Content $log | Select-String "fps\]|IssueSwap" | Select-Object -Last 5 |
  ForEach-Object { $_.Line -replace '^\[\S+ (\S+)\] \[\w+\] \[\w+\] \[t\d+\] ', '$1 ' }
