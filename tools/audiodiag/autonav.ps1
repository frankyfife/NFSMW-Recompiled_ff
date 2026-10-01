param([string]$Name = 'nav', [string]$Keys = 'E*12', [string[]]$Extra = @(), [switch]$Keep, [switch]$Windowed, [int]$Fps = 60)
# Starts the game muted with the raw audio dump, plays a key script and takes
# a screenshot. Keys: comma list of <key>*<count>[@<ms gap>], key in
# E(nter) L(eft) R(ight) U(p) D(own) S(pace) X(Esc) W(ait, count = seconds) P(icture),
# G(as held, count = seconds; V/T/Y/N hold W/arrow up/D/arrow right), F = F6 (free camera).
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class G4 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  // The game's window, not the native renderer's second window.
  public static IntPtr GameWindow(int pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h)) return true;
      var c = new System.Text.StringBuilder(256); GetClassName(h, c, 256);
      if (c.ToString() == "NfsmwNativeRenderer") return true;
      found = h; return false;
    }, IntPtr.Zero);
    return found;
  }
  public struct RECT { public int L, T, R, B; }
}
"@
# Per-monitor aware: window rectangles and the screen copy in physical pixels
# (with only system awareness a 225 % display gave a rectangle past the screen).
if (-not [G4]::SetProcessDpiAwarenessContext([IntPtr](-4))) { [G4]::SetProcessDPIAware() | Out-Null }
# S = Space (A button), B = Backspace (B button), H/J/K/M = stick left/right/up/down (A/D/W/S keys)
$vk = @{ 'E' = 0x0D; 'L' = 0x25; 'R' = 0x27; 'U' = 0x26; 'D' = 0x28; 'S' = 0x20; 'X' = 0x1B;
         'B' = 0x08; 'H' = 0x41; 'J' = 0x44; 'K' = 0x57; 'M' = 0x53; 'F' = 0x75 }
$b = "D:\NFSMW\NFSMW-Recompiled_ff\build"
$dump = "$env:TEMP\claude\audio_$Name.raw"
if (Test-Path $dump) { Remove-Item -LiteralPath $dump }
$log = "$env:TEMP\claude\nav_$Name.log"
$a = @('--game_data_root', "$b\game_root", '--gpu_plugin', 'xenos', "--fullscreen=$(-not $Windowed)".ToLower(),
       '--resolution', '720p', '--guest_vblank_rate=1000', "--frame_pacing_fps=$Fps",
       '--readback_resolve=fast', '--mnk_mode=true', '--log_guest_fps=true', '--audio_mute=true',
       "--audio_dump_file=$dump", '--log_file', $log) + $Extra
$p = Start-Process "$b\nfsmw.exe" -ArgumentList $a -WorkingDirectory $b -PassThru
Start-Sleep -Seconds 8
$p.Refresh()
$h = [G4]::GameWindow($p.Id)
$shot = 0
function Shot {
  $r = New-Object G4+RECT
  [G4]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  $script:shot++
  $f = "$env:TEMP\claude\nav_${Name}_$($script:shot).png"
  $bmp.Save($f); $g.Dispose(); $bmp.Dispose()
  "shot $f"
}
$t0 = Get-Date
foreach ($step in $Keys.Split(',')) {
  $m = [regex]::Match($step.Trim(), '^([A-Z])\*?(\d*)@?(\d*)$')
  $k = $m.Groups[1].Value; $n = if ($m.Groups[2].Value) { [int]$m.Groups[2].Value } else { 1 }
  $gap = if ($m.Groups[3].Value) { [int]$m.Groups[3].Value } else { 1800 }
  if ($k -eq 'W') { Start-Sleep -Seconds $n; continue }
  $hold = @{ 'G' = 0x4F; 'V' = 0x57; 'T' = 0x26; 'Y' = 0x44; 'N' = 0x27 }
  if ($hold.ContainsKey($k)) {
    # Held for n seconds: G = gas (right trigger, O key), V = W key (left
    # stick up), T = arrow up.
    [G4]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 100
    # Arrow keys are extended keys (without the flag they arrive as the keypad).
    $ext = if ($hold[$k] -ge 0x21 -and $hold[$k] -le 0x28) { 1 } else { 0 }
    $scan = [byte][G4]::MapVirtualKey([uint32]$hold[$k], 0)
    [G4]::keybd_event([byte]$hold[$k], $scan, $ext, [UIntPtr]::Zero)
    Start-Sleep -Seconds $n
    [G4]::keybd_event([byte]$hold[$k], $scan, 2 -bor $ext, [UIntPtr]::Zero)
    continue
  }
  if ($k -eq 'P') { Shot; continue }
  for ($i = 0; $i -lt $n; $i++) {
    [G4]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 100
    "{0} key {1}" -f [Diagnostics.Stopwatch]::GetTimestamp(), $k | Add-Content "$env:TEMP\claude\nav_${Name}_keys.txt"
    [G4]::keybd_event([byte]$vk[$k], 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [G4]::keybd_event([byte]$vk[$k], 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $gap
  }
}
Shot
if (-not $Keep) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

