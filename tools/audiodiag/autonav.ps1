param([string]$Name = 'nav', [string]$Keys = 'E*12', [string[]]$Extra = @(), [switch]$Keep, [switch]$Windowed, [int]$Fps = 60, [string]$Resolution = '720p', [string]$Record = '', [int]$RecordFps = 60,
      [string]$UserData = "$env:TEMP\claude\nfsmw_testuser")
# Starts the game muted with the raw audio dump, plays a key script and takes
# a screenshot. Keys: comma list of <key>*<count>[@<ms gap>], key in
# E(nter) L(eft) R(ight) U(p) D(own) S(pace) X(Esc) W(ait, count = seconds) P(icture),
# -Record <mkv>: the screen is recorded (ffmpeg ddagrab, NVENC) from the
# first key on. Held together: <keys joined by +>*<seconds>, e.g. G+d*2
# (gas and steer right); d / a = D / A key held (steer right / left); f / k =
# F / K held (L3 / R3 with mnk_mode, f+k*1 is the free camera chord).
# :<command> = a line typed into the SDK console (e.g. :native_renderer false);
# ;<text> = text typed only; #<hex> / !<hex> = a key by scan code / virtual key.
# s = S key held (count = seconds), F10 = frame time recording, I = Back + Start, O / A = D-pad down / up,
# G(as held, count = seconds; V/T/Y/N/Q/Z hold W/arrow up/D/arrow right/1/3), F = F6 (free camera), C = F8 (photo mode).
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
  // Keys by scan code and text as Unicode characters (SendInput): the same on
  // every keyboard layout (the console key is the one left of 1).
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public INPUTUNION u; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  static void Send(ushort scan, uint flags) {
    var i = new INPUT { type = 1 };
    i.u.ki.wScan = scan;
    i.u.ki.dwFlags = flags;
    SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void ScanKey(ushort scan, bool extended = false) {
    uint e = extended ? 1u : 0u;  // KEYEVENTF_EXTENDEDKEY
    Send(scan, 8 | e);  // KEYEVENTF_SCANCODE
    System.Threading.Thread.Sleep(60);
    Send(scan, 8 | 2 | e);
  }
  public static void TypeText(string text) {
    foreach (char c in text) {
      Send(c, 4);  // KEYEVENTF_UNICODE
      Send(c, 4 | 2);
      System.Threading.Thread.Sleep(20);
    }
  }
}
"@
# Per-monitor aware: window rectangles and the screen copy in physical pixels
# (with only system awareness a 225 % display gave a rectangle past the screen).
if (-not [G4]::SetProcessDpiAwarenessContext([IntPtr](-4))) { [G4]::SetProcessDPIAware() | Out-Null }
# S = Space (A button), B = Backspace (B button), H/J/K/M = stick left/right/up/down (A/D/W/S keys)
$vk = @{ 'E' = 0x0D; 'L' = 0x25; 'R' = 0x27; 'U' = 0x26; 'D' = 0x28; 'S' = 0x20; 'X' = 0x1B;
         'B' = 0x08; 'YBUTTON' = 0x50; 'H' = 0x41; 'J' = 0x44; 'K' = 0x57; 'M' = 0x53; 'F' = 0x75; 'C' = 0x77 }
$b = "D:\NFSMW\NFSMW-Recompiled_ff\build"
$dump = "$env:TEMP\claude\audio_$Name.raw"
if (Test-Path $dump) { Remove-Item -LiteralPath $dump }
$log = "$env:TEMP\claude\nav_$Name.log"
# The game's saves (profiles) of the tests live apart from the player's
# (Documents\nfsmw): a copy of them, made once. Key sequences that create or
# change a profile then never touch the player's.
if (-not (Test-Path $UserData)) {
  New-Item -ItemType Directory -Force $UserData | Out-Null
  $saves = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'nfsmw'
  Get-ChildItem $saves -Directory | Where-Object { $_.Name -notmatch '^(cache|_backup)' } |
    ForEach-Object { Copy-Item $_.FullName -Destination $UserData -Recurse }
}
$a = @('--game_data_root', "$b\game_root", "--user_data_root=$UserData", '--gpu_plugin', 'xenos',
       "--fullscreen=$(-not $Windowed)".ToLower(),
       '--resolution', $Resolution, '--guest_vblank_rate=1000', "--frame_pacing_fps=$Fps",
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
$rec = $null
if ($Record) {
  $ff = 'D:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe'
  # Started through .NET: Start-Process with a hidden window did not return here.
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $ff
  $psi.Arguments = "-y -v error -f lavfi -i ddagrab=output_idx=0:framerate=$($RecordFps):draw_mouse=0 -c:v h264_nvenc -cq 24 `"$Record`""
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardInput = $true  # q on stdin ends the recording cleanly
  $rec = [System.Diagnostics.Process]::Start($psi)
}
$holdKeys = @{ 'BACK' = 0x53; 'G' = 0x4F; 'V' = 0x57; 'T' = 0x26; 'Y' = 0x44; 'N' = 0x27;
               'Q' = 0x31; 'Z' = 0x33; 'd' = 0x44; 'a' = 0x41; 'f' = 0x46; 'k' = 0x4B }
foreach ($step in $Keys.Split(',')) {
  if ($step -match '^([#!])([0-9A-Fa-f]{2})$') {
    # #<hex> = a key by scan code, !<hex> = by virtual key code.
    [G4]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 100
    $code = [Convert]::ToUInt16($Matches[2], 16)
    if ($Matches[1] -eq '#') {
      [G4]::ScanKey($code)
    } else {
      [G4]::keybd_event([byte]$code, [byte][G4]::MapVirtualKey([uint32]$code, 0), 0, [UIntPtr]::Zero)
      Start-Sleep -Milliseconds 60
      [G4]::keybd_event([byte]$code, [byte][G4]::MapVirtualKey([uint32]$code, 0), 2, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 700
    continue
  }
  if ($step.StartsWith(';')) {
    # ;<text> = only typed (as Unicode characters), e.g. into an open console.
    [G4]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 100
    [G4]::TypeText($step.Substring(1)); Start-Sleep -Milliseconds 300
    continue
  }
  if ($step.StartsWith(':')) {
    # :<command> = a line in the SDK console (opened and closed with the key
    # left of 1), e.g. ":native_renderer false". Backspaces first: the console
    # key is a dead key on some layouts.
    [G4]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 100
    [G4]::ScanKey(0x29); Start-Sleep -Milliseconds 500
    [G4]::ScanKey(0x0E); [G4]::ScanKey(0x0E); Start-Sleep -Milliseconds 100
    [G4]::TypeText($step.Substring(1)); Start-Sleep -Milliseconds 200
    # The keypad's Enter (the game still sees an Enter as Start with
    # mnk_mode and pauses in free roam: follow with E to resume).
    [G4]::ScanKey(0x1C, $true); Start-Sleep -Milliseconds 500
    [G4]::ScanKey(0x29); Start-Sleep -Milliseconds 500
    continue
  }
  $mm = [regex]::Match($step.Trim(), '^([A-Za-z](?:\+[A-Za-z])+)\*(\d+)$')
  if ($mm.Success) {
    # Several keys held together for n seconds.
    $vks = @()
    foreach ($ch in $mm.Groups[1].Value.Split('+')) {
      $key = if ('d', 'a', 'f', 'k' -ccontains $ch) { $ch } else { $ch.ToUpper() }
      foreach ($kk in $holdKeys.Keys) { if ($kk -ceq $key) { $vks += $holdKeys[$kk] } }
    }
    [G4]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 100
    foreach ($v in $vks) {
      $ext = if ($v -ge 0x21 -and $v -le 0x28) { 1 } else { 0 }
      [G4]::keybd_event([byte]$v, [byte][G4]::MapVirtualKey([uint32]$v, 0), $ext, [UIntPtr]::Zero)
    }
    Start-Sleep -Seconds ([int]$mm.Groups[2].Value)
    foreach ($v in $vks) {
      $ext = if ($v -ge 0x21 -and $v -le 0x28) { 1 } else { 0 }
      [G4]::keybd_event([byte]$v, [byte][G4]::MapVirtualKey([uint32]$v, 0), 2 -bor $ext, [UIntPtr]::Zero)
    }
    continue
  }
  if ($step.Trim() -eq 'F10') {
    # F10: frame time recording on/off.
    [G4]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 100
    [G4]::keybd_event(0x79, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [G4]::keybd_event(0x79, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 500
    continue
  }
  $m = [regex]::Match($step.Trim(), '^([A-Za-z])\*?(\d*)@?(\d*)$')
  $k = $m.Groups[1].Value; $n = if ($m.Groups[2].Value) { [int]$m.Groups[2].Value } else { 1 }
  $gap = if ($m.Groups[3].Value) { [int]$m.Groups[3].Value } else { 1800 }
  if ($k -eq 'W') { Start-Sleep -Seconds $n; continue }
  # Lower case s: S key held (left stick down: free camera backwards).
  if ($k -ceq 's') { $k = 'BACK' }
  if ($k -ceq 'y') { $k = 'YBUTTON' }  # the P key: Y with mnk_mode (P is the shot)
  $hold = @{ 'BACK' = 0x53; 'G' = 0x4F; 'V' = 0x57; 'T' = 0x26; 'Y' = 0x44; 'N' = 0x27; 'Q' = 0x31; 'Z' = 0x33 }
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
  # Chords (pressed together, in order, released in reverse): I = Tab + Return
  # (Back + Start), O / A = Shift + arrow down / up (D-pad down / up).
  $chord = @{ 'I' = @(0x09, 0x0D); 'O' = @(0x10, 0x28); 'A' = @(0x10, 0x26) }
  if ($chord.ContainsKey($k)) {
    for ($i = 0; $i -lt $n; $i++) {
      [G4]::SetForegroundWindow($h) | Out-Null
      Start-Sleep -Milliseconds 100
      foreach ($c in $chord[$k]) {
        $ext = if ($c -ge 0x21 -and $c -le 0x28) { 1 } else { 0 }
        [G4]::keybd_event([byte]$c, [byte][G4]::MapVirtualKey([uint32]$c, 0), $ext, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 60
      }
      Start-Sleep -Milliseconds 150
      foreach ($c in $chord[$k][($chord[$k].Count - 1)..0]) {
        $ext = if ($c -ge 0x21 -and $c -le 0x28) { 1 } else { 0 }
        [G4]::keybd_event([byte]$c, [byte][G4]::MapVirtualKey([uint32]$c, 0), 2 -bor $ext, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 30
      }
      Start-Sleep -Milliseconds $gap
    }
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
$p.Refresh()
if ($rec) {
  $rec.StandardInput.Write('q')
  $rec.StandardInput.Flush()
  if (-not $rec.WaitForExit(20000)) { Stop-Process -Id $rec.Id -ErrorAction SilentlyContinue }
}
if ($p.HasExited) { "game exited (code $($p.ExitCode))"; return }
Shot
if (-not $Keep) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }

