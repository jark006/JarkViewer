# Capture (and drive) a single window of the launched app, for visual verification.
#
# Usage examples:
#   pwsh tools/capture_window.ps1 -Exe x64/Release/JarkViewer.exe -Argument "img.svg" -Out shot.png
#   pwsh tools/capture_window.ps1 -Exe ... -Argument "img.png" -Keys "{F1}" -Window smallest -Out settings.png
#   pwsh tools/capture_window.ps1 -Exe ... -Keys "{F1}" -Window smallest -Click "41,95" -Out clicked.png
#   pwsh tools/capture_window.ps1 -Exe ... -Argument "img.png" -Hover "20,300" -Out hover.png
#
# -Window main      : main window (default)
# -Window smallest  : smallest visible window (e.g. the settings window opened by F1)
# -Window child     : smallest visible window that is not the main one (settings/editor/batch)
# -Click "x,y"      : click at logical client coordinates; physical scale is derived from
#                     the window width and -LogicWidth (default 1000)
#
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Argument = "",
    [string]$Keys = "",
    [ValidateSet("main", "smallest", "child")]
    [string]$Window = "main",
    [string]$Click = "",
    [string]$Hover = "",
    [string]$Drag = "",
    [int]$SegmentDelayMs = -1,   # delay between drag segments (default: $AfterKeysMs)
    [int]$LogicWidth = 1000,
    [int]$WaitMs = 2500,
    [int]$AfterKeysMs = 1200,
    [int]$TimeoutMs = 15000,
    [switch]$Screen      # grab from the screen instead of PrintWindow (verifies what is actually shown)
)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public class JarkCapture {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool attachFlag);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();

    // SetForegroundWindow only works for processes owning the foreground; attaching the
    // input queues first makes it reliable from a background script.
    public static bool ForceForeground(IntPtr hwnd) {
        if (GetForegroundWindow() == hwnd) return true;

        uint targetThread = GetWindowThreadProcessId(hwnd, out uint _pid);
        uint currentThread = GetCurrentThreadId();
        IntPtr foreground = GetForegroundWindow();
        bool attachedForeground = false, attachedTarget = false;

        if (foreground != IntPtr.Zero) {
            uint fgThread = GetWindowThreadProcessId(foreground, out uint _fgPid);
            if (fgThread != currentThread) attachedForeground = AttachThreadInput(currentThread, fgThread, true);
        }
        if (targetThread != currentThread) attachedTarget = AttachThreadInput(currentThread, targetThread, true);

        ShowWindow(hwnd, 9); // SW_RESTORE
        BringWindowToTop(hwnd);
        bool ok = SetForegroundWindow(hwnd);

        if (attachedTarget) AttachThreadInput(currentThread, targetThread, false);
        if (attachedForeground) AttachThreadInput(currentThread, GetWindowThreadProcessId(foreground, out uint _f2), false);
        return ok || GetForegroundWindow() == hwnd;
    }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }

    public static List<IntPtr> Windows(int pid) {
        var list = new List<IntPtr>();
        EnumWindows(delegate(IntPtr hwnd, IntPtr param) {
            uint p; GetWindowThreadProcessId(hwnd, out p);
            if (p == (uint)pid && IsWindowVisible(hwnd)) list.Add(hwnd);
            return true;
        }, IntPtr.Zero);
        return list;
    }

    // Smallest visible window of the process that is not in "exclude" (pass the HWNDs
    // that existed before the dialog was opened; the new dialog is what is left).
    public static IntPtr SmallestWindow(int pid, List<IntPtr> exclude) {
        IntPtr best = IntPtr.Zero; long bestArea = long.MaxValue;
        foreach (IntPtr hwnd in Windows(pid)) {
            if (exclude != null && exclude.Contains(hwnd)) continue;
            RECT r; GetWindowRect(hwnd, out r);
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > 0 && area < bestArea) { bestArea = area; best = hwnd; }
        }
        return best;
    }
}
"@

# The host must be DPI-aware, otherwise GetWindowRect reports virtualized (logical)
# coordinates and the capture ends up as a downscaled top-left crop of a high-DPI window.
[void][JarkCapture]::SetProcessDPIAware()

function Activate-Window([IntPtr]$hwnd) {
    for ($i = 0; $i -lt 10; $i++) {
        if ([JarkCapture]::ForceForeground($hwnd)) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

$proc = Start-Process -FilePath $Exe -ArgumentList $Argument -PassThru
try {
    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $proc.Refresh()
        if ($proc.HasExited) { throw "process exited early with code $($proc.ExitCode)" }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle; break }
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "main window not found within $TimeoutMs ms" }

    Start-Sleep -Milliseconds $WaitMs

    # Windows that exist before the dialogs open; "child" mode picks whatever is left.
    $initialWindows = [JarkCapture]::Windows($proc.Id)

    if ($Keys -ne "") {
        [void](Activate-Window $hwnd)
        [System.Windows.Forms.SendKeys]::SendWait($Keys)
        Start-Sleep -Milliseconds $AfterKeysMs
    }

    if ($Window -eq "smallest" -or $Window -eq "child") {
        # "child" = smallest window that appeared after launch (settings / editor / batch dialog)
        $exclude = if ($Window -eq "child") { $initialWindows } else { $null }
        $target = [JarkCapture]::SmallestWindow($proc.Id, $exclude)
        if ($target -ne [IntPtr]::Zero) {
            $hwnd = $target
            Start-Sleep -Milliseconds 400
        }
    }

    if ($Click -ne "") {
        [void](Activate-Window $hwnd)

        $windowRect = New-Object JarkCapture+RECT
        [void][JarkCapture]::GetWindowRect($hwnd, [ref]$windowRect)
        $scale = ($windowRect.Right - $windowRect.Left) / [double]$LogicWidth

        $parts = $Click.Split(",")
        $point = New-Object JarkCapture+POINT
        $point.X = [int]([int]$parts[0] * $scale)
        $point.Y = [int]([int]$parts[1] * $scale)
        [void][JarkCapture]::ClientToScreen($hwnd, [ref]$point)

        [void][JarkCapture]::SetCursorPos($point.X, $point.Y)
        Start-Sleep -Milliseconds 200
        [JarkCapture]::mouse_event([JarkCapture]::LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 60
        [JarkCapture]::mouse_event([JarkCapture]::LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds $AfterKeysMs
    }

    if ($Hover -ne "") {
        # Hover without clicking: physical client coordinates (for the DPI-aware main window,
        # client pixels == physical pixels), used to check hover-only overlays.
        [void](Activate-Window $hwnd)

        $parts = $Hover.Split(",")
        $point = New-Object JarkCapture+POINT
        $point.X = [int]$parts[0]
        $point.Y = [int]$parts[1]
        [void][JarkCapture]::ClientToScreen($hwnd, [ref]$point)

        [void][JarkCapture]::SetCursorPos($point.X, $point.Y)
        # Nudge so a WM_MOUSEMOVE is definitely delivered even if the cursor was already there.
        [void][JarkCapture]::SetCursorPos($point.X + 1, $point.Y + 1)
        Start-Sleep -Milliseconds 80
        [void][JarkCapture]::SetCursorPos($point.X, $point.Y)
        Start-Sleep -Milliseconds $AfterKeysMs
    }

    if ($Drag -ne "") {
        # Left-button drags in physical client coordinates, "x1,y1,x2,y2" per segment,
        # segments separated by ";". Moved in steps so intermediate WM_MOUSEMOVE arrive;
        # an empty segment (same start/end) acts as a plain click.
        [void](Activate-Window $hwnd)

        foreach ($segment in $Drag.Split(";")) {
            $parts = $segment.Split(",")
            $from = New-Object JarkCapture+POINT
            $from.X = [int]$parts[0]
            $from.Y = [int]$parts[1]
            $to = New-Object JarkCapture+POINT
            $to.X = [int]$parts[2]
            $to.Y = [int]$parts[3]
            [void][JarkCapture]::ClientToScreen($hwnd, [ref]$from)
            [void][JarkCapture]::ClientToScreen($hwnd, [ref]$to)

            [void][JarkCapture]::SetCursorPos($from.X, $from.Y)

            if ($from.X -eq $to.X -and $from.Y -eq $to.Y) {
                # plain click: keep it short so repeated segments register as a double click
                Start-Sleep -Milliseconds 60
                [JarkCapture]::mouse_event([JarkCapture]::LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
                Start-Sleep -Milliseconds 60
            }
            else {
                Start-Sleep -Milliseconds 150
                [JarkCapture]::mouse_event([JarkCapture]::LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
                for ($step = 1; $step -le 6; $step++) {
                    Start-Sleep -Milliseconds 60
                    [void][JarkCapture]::SetCursorPos(
                        [int]($from.X + ($to.X - $from.X) * $step / 6),
                        [int]($from.Y + ($to.Y - $from.Y) * $step / 6))
                }
                Start-Sleep -Milliseconds 100
            }

            [JarkCapture]::mouse_event([JarkCapture]::LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
            if ($SegmentDelayMs -ge 0) { Start-Sleep -Milliseconds $SegmentDelayMs }
            else { Start-Sleep -Milliseconds $AfterKeysMs }
        }
    }

    $rect = New-Object JarkCapture+RECT
    if (-not [JarkCapture]::GetWindowRect($hwnd, [ref]$rect)) { throw "GetWindowRect failed" }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { throw "invalid window size ${width}x${height}" }

    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    if ($Screen) {
        [void](Activate-Window $hwnd)
        Start-Sleep -Milliseconds 400
        # 最大化窗口的 Left/Top 可能是负数，取屏幕内可见部分
        $srcX = [Math]::Max(0, $rect.Left)
        $srcY = [Math]::Max(0, $rect.Top)
        $visibleW = [Math]::Min($width, [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width - $srcX)
        $visibleH = [Math]::Min($height, [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height - $srcY)
        $graphics.CopyFromScreen($srcX, $srcY, $srcX - $rect.Left, $srcY - $rect.Top,
            (New-Object System.Drawing.Size($visibleW, $visibleH)))
        $printed = $true
    }
    else {
        $hdc = $graphics.GetHdc()
        $printed = [JarkCapture]::PrintWindow($hwnd, $hdc, 2)   # PW_RENDERFULLCONTENT
        $graphics.ReleaseHdc($hdc)
    }
    $graphics.Dispose()

    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()

    "captured=$printed size=${width}x${height} out=$Out"
}
finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
