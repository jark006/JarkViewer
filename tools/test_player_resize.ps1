# Regression check for the video player: after resizing the window the picture must
# still be there. The paused case is the one that used to break: a resize recreates
# the staging texture (empty) and there is no "next frame" to repaint it, so the whole
# window went black until something else forced a redraw.
#
#   pwsh tools/test_player_resize.ps1 -Exe x64/Release/JarkViewer.exe -Video <file> -OutDirectory <dir>
#
# Passes when the mean luma of the picture area stays high after shrink+grow while paused.
# Keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Video,
    [Parameter(Mandatory = $true)][string]$OutDirectory
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class JvRs { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx); }
"@
try { [void][JvRs]::SetProcessDpiAwarenessContext([IntPtr](-4)) } catch { }
Add-Type @"
using System; using System.Runtime.InteropServices;
public class JvRs2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public UIntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
  static void Abs(int sx, int sy, uint flag) {
    int cx = GetSystemMetrics(0), cy = GetSystemMetrics(1);
    var inp = new INPUT[1];
    inp[0].type = 0;
    inp[0].mi.dx = (int)Math.Round((sx + 0.5) * 65535.0 / cx);
    inp[0].mi.dy = (int)Math.Round((sy + 0.5) * 65535.0 / cy);
    inp[0].mi.dwFlags = 0x0001 | 0x8000 | flag;
    SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Click(int x, int y) { Abs(x, y, 0x0002); System.Threading.Thread.Sleep(40); Abs(x, y, 0x0004); }
  public static IntPtr Main(uint pid) {
    IntPtr best = IntPtr.Zero; long bestArea = 0;
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p != pid || !IsWindowVisible(h)) return true;
      RECT r; GetWindowRect(h, out r);
      long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
      if (a > bestArea) { bestArea = a; best = h; }
      return true;
    }, IntPtr.Zero);
    return best;
  }
}
"@

function Grab([IntPtr]$hwnd, [string]$path) {
    $r = New-Object JvRs2+RECT
    [void][JvRs2]::GetWindowRect($hwnd, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap(($r.Right - $r.Left), ($r.Bottom - $r.Top))
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][JvRs2]::PrintWindow($hwnd, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    return ,$bmp
}

# Mean luma over the middle of the picture (0 = all black).
function MeanLuma([System.Drawing.Bitmap]$bmp) {
    $sum = 0.0; $n = 0
    for ($y = [int]($bmp.Height * 0.2); $y -lt [int]($bmp.Height * 0.6); $y += 9) {
        for ($x = [int]($bmp.Width * 0.2); $x -lt [int]($bmp.Width * 0.8); $x += 9) {
            $c = $bmp.GetPixel($x, $y)
            $sum += 0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B
            $n++
        }
    }
    if ($n -eq 0) { return -1 }
    return $sum / $n
}

New-Item -ItemType Directory -Force -Path $OutDirectory | Out-Null
$proc = Start-Process -FilePath $Exe -ArgumentList @($Video) -PassThru
Start-Sleep -Seconds 3
$hwnd = [JvRs2]::Main([uint32]$proc.Id)
if ($hwnd -eq [IntPtr]::Zero) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; throw "player window not found" }
[void][JvRs2]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 400
# The player inherits the viewer's last geometry (often maximized); restore so the
# window can actually be resized.
[void][JvRs2]::ShowWindow($hwnd, 9)  # SW_RESTORE
Start-Sleep -Milliseconds 500

$cr = New-Object JvRs2+RECT; [void][JvRs2]::GetClientRect($hwnd, [ref]$cr)
$wr = New-Object JvRs2+RECT; [void][JvRs2]::GetWindowRect($hwnd, [ref]$wr)
Write-Output ("window {0}x{1} client {2}x{3}" -f ($wr.Right-$wr.Left), ($wr.Bottom-$wr.Top), ($cr.Right-$cr.Left), ($cr.Bottom-$cr.Top))

# Pause: click once on the picture (the player starts playing).
$c = New-Object JvRs2+POINT
$c.X = [int](($cr.Right - $cr.Left) / 2); $c.Y = [int](($cr.Bottom - $cr.Top) / 2)
[void][JvRs2]::ClientToScreen($hwnd, [ref]$c)
[JvRs2]::Click($c.X, $c.Y)
Start-Sleep -Milliseconds 900

$before = Grab $hwnd (Join-Path $OutDirectory "player-resize-before.png")
$lBefore = MeanLuma $before
$before.Dispose()

$curW = $wr.Right - $wr.Left; $curH = $wr.Bottom - $wr.Top
foreach ($delta in @(-180, 260)) {
    $curW += $delta; $curH += $delta
    [void][JvRs2]::SetWindowPos($hwnd, [IntPtr]::Zero, 60, 60, $curW, $curH, 0x0004)
    Start-Sleep -Milliseconds 900
    $now = New-Object JvRs2+RECT; [void][JvRs2]::GetWindowRect($hwnd, [ref]$now)
    Write-Output ("resized delta={0} -> window {1}x{2}" -f $delta, ($now.Right-$now.Left), ($now.Bottom-$now.Top))
}
$after = Grab $hwnd (Join-Path $OutDirectory "player-resize-after.png")
$lAfter = MeanLuma $after
$after.Dispose()

Write-Output ("paused mean luma: before {0:N1} -> after {1:N1}" -f $lBefore, $lAfter)
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue

if ($lAfter -gt 30) {
    Write-Output "PASS: picture survives a resize while paused (not a black window)"
    exit 0
}
Write-Output "FAIL: resizing while paused left the window black"
exit 1
