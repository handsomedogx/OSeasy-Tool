# ============================================================================
#  check.ps1 - OeWinFix2 regression asserts (ASCII only: Windows PowerShell 5.1
#  reads .ps1 as ANSI, so non-ASCII text in scripts is asking for trouble.)
#
#  Asserts:
#   A. client lock reproduced: FakeBox (real StudentLogic.dll hook) running ->
#      injected extraInfo=0 must be swallowed (moved == 0), 0x12452 must pass (> 0)
#   B. windowize works: fake broadcast window loses WS_POPUP / WS_EX_TOPMOST,
#      gains WS_CAPTION, and is no longer full-screen sized
#   C. with relay engaged, the whitelisted path (0x12452) still moves the cursor
#      (regression: relay used to freeze even 0x12452 and pin the pointer in a corner)
#   D. log evidence: relay state ON, ignored injected packets > 0 (hDevice filter),
#      keyboard release IOCTL OK
#
#  usage: powershell -ExecutionPolicy Bypass -File test\check.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$dev  = Join-Path $bin 'dev'          # dev tools live here; bin\ keeps only the program
$log  = Join-Path $bin 'OeWinFix2.log'
$result = New-Object System.Collections.ArrayList

function Assert($name, $ok, $detail) {
    [void]$result.Add([pscustomobject]@{ Test = $name; Pass = $ok; Detail = $detail })
    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1}  --  {2}" -f $tag, $name, $detail)
}

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
  // Struct size must match the platform: on x64 INPUT is 40 bytes. Getting this
  // wrong makes SendInput return 0 (that produced a false "moved=0" in v1 of this script).
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {
      public int dx, dy; public uint mouseData, dwFlags, time; public UIntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] ins, int size);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int idx);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static IntPtr FindByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      StringBuilder sb = new StringBuilder(128);
      GetClassNameW(h, sb, 128);
      if (string.Equals(sb.ToString(), cls, StringComparison.OrdinalIgnoreCase)) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static int InputSize() { return Marshal.SizeOf(typeof(INPUT)); }
  public static long MoveSteps(int steps, uint extra) {
    POINT p0; GetCursorPos(out p0);
    for (int i = 0; i < steps; i++) {
      INPUT[] inb = new INPUT[1];
      inb[0].type = 0; inb[0].mi.dx = 12; inb[0].mi.dy = 8; inb[0].mi.dwFlags = 0x0001; // MOUSEEVENTF_MOVE
      inb[0].mi.dwExtraInfo = (UIntPtr)extra;
      uint sent = SendInput(1, inb, Marshal.SizeOf(typeof(INPUT)));
      if (sent == 0 && i == 0)
        throw new Exception("SendInput rejected (INPUT size=" + Marshal.SizeOf(typeof(INPUT)) + ", err=" + Marshal.GetLastWin32Error() + ")");
    }
    System.Threading.Thread.Sleep(250);
    POINT p1; GetCursorPos(out p1);
    return Math.Abs(p1.x - p0.x) + Math.Abs(p1.y - p0.y);
  }
}
'@

Write-Host "=== OeWinFix2 regression check ==="

# Precondition: no stale instances (the single-instance mutex would block a new one)
Get-Process OeWinFix2, FakeBox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item $log -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- A. client lock
Remove-Item (Join-Path $dev 'FakeBox.out') -ErrorAction SilentlyContinue
$fb = Start-Process -FilePath (Join-Path $dev 'FakeBox.exe') `
                    -ArgumentList 'lock','--live','--inject','0' `
                    -WorkingDirectory $root -PassThru `
                    -RedirectStandardOutput (Join-Path $dev 'FakeBox.out') `
                    -RedirectStandardError  (Join-Path $dev 'FakeBox.err')
Start-Sleep -Seconds 3
$fakeWnd = [T]::FindByClass('MultiRenderWindowClass')
Assert 'A0 fake broadcast window exists' ($fakeWnd -ne [IntPtr]::Zero) `
       ("hwnd=0x{0:X}  INPUT size={1}" -f [int64]$fakeWnd, [T]::InputSize())
if ($fakeWnd -eq [IntPtr]::Zero) {
    Write-Host '--- FakeBox stdout ---'
    Get-Content (Join-Path $dev 'FakeBox.out') -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }
    Get-Content (Join-Path $dev 'FakeBox.err') -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }
}

$plain = [T]::MoveSteps(6, 0)
$magic = [T]::MoveSteps(6, 0x12452)
Assert 'A1 plain event swallowed by client hook (moved=0)' ($plain -eq 0) ("moved=$plain")
Assert 'A2 whitelisted 0x12452 passes (moved>0)'          ($magic -gt 5) ("moved=$magic")

# ---------------------------------------------------------------- B. windowize
$style0 = [T]::GetWindowLongW($fakeWnd, -16)
$ex0    = [T]::GetWindowLongW($fakeWnd, -20)
$r0 = New-Object T+RECT; [void][T]::GetWindowRect($fakeWnd, [ref]$r0)

$ow = Start-Process -FilePath (Join-Path $bin 'OeWinFix2.exe') `
                    -ArgumentList '--no-desktop','--no-mirror' `
                    -WorkingDirectory $bin -PassThru
Start-Sleep -Seconds 5

$style1 = [T]::GetWindowLongW($fakeWnd, -16)
$ex1    = [T]::GetWindowLongW($fakeWnd, -20)
$r1 = New-Object T+RECT; [void][T]::GetWindowRect($fakeWnd, [ref]$r1)
$w0 = $r0.r - $r0.l; $h0 = $r0.b - $r0.t
$w1 = $r1.r - $r1.l; $h1 = $r1.b - $r1.t
Assert 'B1 WS_EX_TOPMOST cleared'  (($ex1 -band 0x8) -eq 0)          ("ex 0x{0:X} -> 0x{1:X}" -f $ex0, $ex1)
Assert 'B2 WS_POPUP cleared'       (($style1 -band 0x80000000) -eq 0) ("style 0x{0:X} -> 0x{1:X}" -f $style0, $style1)
Assert 'B3 WS_CAPTION added'       (($style1 -band 0x00C00000) -eq 0x00C00000) ("style=0x{0:X}" -f $style1)
Assert 'B4 no longer fullscreen'   (($w1 -lt $w0) -or ($h1 -lt $h0)) ("{0}x{1} -> {2}x{3}" -f $w0,$h0,$w1,$h1)

# ---------------------------------------------------------------- C. relay engaged, whitelisted path alive
$magic2 = [T]::MoveSteps(6, 0x12452)
Assert 'C1 0x12452 still passes while relay engaged' ($magic2 -gt 5) ("moved=$magic2 (before relay: $magic)")

# ---------------------------------------------------------------- D. log evidence
Start-Sleep -Seconds 7          # relay 每 5 秒写一条统计行，等它落盘
$text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
$relayOn = $text -match 'relay: state -> ON'
if (-not $relayOn) { $relayOn = $text -match '\u72b6\u6001 -> \u63a5\u7ba1\u4e2d' }   # state -> ON (Chinese wording)
Assert 'D1 relay engaged (broadcast detected)' $relayOn 'log: relay state -> ON'
$ignored = -1
if ($text -match '\u5ffd\u7565\u6ce8\u5165\u5305 (\d+)') { $ignored = [int]$matches[1] }  # ignored injected packets
Assert 'D2 injected packets ignored (hDevice filter, no feedback loop)' ($ignored -gt 0) ("ignored=$ignored")
$kbdOk = $text -match 'release IOCTL 0x000B2008 OK'
Assert 'D3 keyboard release IOCTL OK' $kbdOk 'log: keyboard release OK'

# ---------------------------------------------------------------- cleanup
if ($ow -and -not $ow.HasExited) { Stop-Process -Id $ow.Id -Force -ErrorAction SilentlyContinue }
if ($fb -and -not $fb.HasExited) { Stop-Process -Id $fb.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 500

Write-Host ''
$failedRows = @($result | Where-Object { $_.Pass -eq $false })
$failed = $failedRows.Count
Write-Host ("=== result: {0}/{1} passed ===" -f ($result.Count - $failed), $result.Count)
if ($failed -gt 0) {
    Write-Host 'failed:'
    $failedRows | ForEach-Object { Write-Host ("  - " + $_.Test + " :: " + $_.Detail) }
}
exit $failed
