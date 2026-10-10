# Regression check for the video player: the picture must be scaled with a real filter, not
# nearest neighbour. Play a black/white checkerboard video (losslessly encoded, so the decoded
# frame is exactly 0/255) in a window several times bigger than the frame: the scaled picture
# must show intermediate luma values along every square edge. Pure nearest-neighbour upscaling
# reproduces only 0/255 -- the screenshot has ~2 luma levels and the picture looks like blocks
# ("grainy/pixelated"), which is exactly what the pre-scale-to-screen-size step fixes.
#
#   pwsh tools/test_player_scaling.ps1 -Exe x64/Release/JarkViewer.exe -Video <checker.mp4> -OutDirectory <dir>
#
# Test video (regenerate anywhere; needs ffmpeg):
#   ffmpeg -f lavfi -i "nullsrc=s=320x240:d=4:r=10,format=yuv444p,geq=lum='if(eq(mod(floor(X/8)+floor(Y/8),2),0),0,255)':cb=128:cr=128" \
#          -c:v libx264 -qp 0 -pix_fmt yuv444p checker-320x240-lossless.mp4
#
# PowerShell 7 (pwsh) only -- the guard below refuses Windows PowerShell 5.1 and says how to
# install pwsh. Keep the UTF-8 BOM on this file: 5.1 decodes BOM-less .ps1 as ANSI, which mangles
# the script before the guard can run (it dies with a syntax error instead of the hint).


param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Video,
    [Parameter(Mandatory = $true)][string]$OutDirectory,
    [int]$Width = 1180,
    [int]$Height = 820
)

# --- PowerShell 7 (pwsh) only -------------------------------------------------------------
# Windows PowerShell 5.1 is refused below: it reads BOM-less .ps1 files as ANSI (mojibake, and
# sometimes a syntax error that hides this guard) and quotes Start-Process arguments
# differently, which the UI test scripts depend on. Run everything with pwsh.
if ($PSVersionTable.PSEdition -ne 'Core') {
    Write-Host "This script requires PowerShell 7 (pwsh); Windows PowerShell $($PSVersionTable.PSVersion) is not supported." -ForegroundColor Red
    Write-Host "Re-run with:  pwsh -File `"$PSCommandPath`"" -ForegroundColor Yellow
    Write-Host "Install:      winget install --id Microsoft.PowerShell" -ForegroundColor Yellow
    exit 1
}

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class JvSc { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx); }
"@
try { [void][JvSc]::SetProcessDpiAwarenessContext([IntPtr](-4)) } catch { }
Add-Type @"
using System; using System.Runtime.InteropServices;
public class JvSc2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool AdjustWindowRectEx(ref RECT r, uint style, bool menu, uint exStyle);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int index);
  [DllImport("user32.dll")] public static extern int GetDpiForWindow(IntPtr h);
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

function Grab([IntPtr]$h, [string]$path) {
    $r = New-Object JvSc2+RECT; [void][JvSc2]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap(($r.Right - $r.Left), ($r.Bottom - $r.Top))
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][JvSc2]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    return ,$bmp
}

New-Item -ItemType Directory -Force -Path $OutDirectory | Out-Null
$proc = Start-Process -FilePath $Exe -ArgumentList @($Video) -PassThru
Start-Sleep -Seconds 3
$hwnd = [JvSc2]::Main([uint32]$proc.Id)
if ($hwnd -eq [IntPtr]::Zero) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; throw "player window not found" }
[void][JvSc2]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 400
# The player inherits the viewer's last geometry (often maximized); restore and pin the size so
# the upscale factor is deterministic.
[void][JvSc2]::ShowWindow($hwnd, 9)  # SW_RESTORE
Start-Sleep -Milliseconds 300
$rc = New-Object JvSc2+RECT
$rc.Left = 0; $rc.Top = 0; $rc.Right = $Width; $rc.Bottom = $Height
$style = [JvSc2]::GetWindowLong($hwnd, -16); $exStyle = [JvSc2]::GetWindowLong($hwnd, -20)
[void][JvSc2]::AdjustWindowRectEx([ref]$rc, [uint32]$style, $false, [uint32]$exStyle)
$winW = $rc.Right - $rc.Left; $winH = $rc.Bottom - $rc.Top
[void][JvSc2]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, $winW, $winH, 0x0004)
Start-Sleep -Milliseconds 1200

$cr = New-Object JvSc2+RECT; [void][JvSc2]::GetClientRect($hwnd, [ref]$cr)
$cw = $cr.Right - $cr.Left; $ch = $cr.Bottom - $cr.Top
$scale = [JvSc2]::GetDpiForWindow($hwnd) / 96.0
Write-Output ("client {0}x{1} dpi scale {2:N2}" -f $cw, $ch, $scale)

$bmp = Grab $hwnd (Join-Path $OutDirectory "player-scaling.png")

# Sample the middle of the picture area (the fitted picture always covers the centre; the
# letterbox bars are at the sides and the progress bar at the bottom).
$levels = New-Object 'System.Collections.Generic.HashSet[int]'
$mid = 0; $n = 0
for ($y = [int]($bmp.Height * 0.15); $y -lt [int]($bmp.Height * 0.55); $y += 2) {
    for ($x = [int]($bmp.Width * 0.25); $x -lt [int]($bmp.Width * 0.75); $x += 2) {
        $c = $bmp.GetPixel($x, $y)
        $l = [int][Math]::Round(0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B)
        [void]$levels.Add($l)
        if ($l -gt 12 -and $l -lt 243) { $mid++ }
        $n++
    }
}
$bmp.Dispose()
$midPct = 100.0 * $mid / $n
Write-Output ("samples {0}  distinct luma {1}  intermediate {2:N2}%" -f $n, $levels.Count, $midPct)
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue

if ($levels.Count -ge 24 -and $midPct -ge 4.0) {
    Write-Output "PASS: the upscaled picture is filtered (edge pixels have intermediate luma)"
    exit 0
}
Write-Output "FAIL: upscaling looks like nearest neighbour (blocky, ~2 luma levels)"
exit 1
