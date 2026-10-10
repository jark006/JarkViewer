# Verify that the player window and the image window share ONE window-placement memory:
# resize/position the window in one mode, quit, reopen in the other mode -- the geometry
# must be inherited (that is the intended design, see AGENTS.md "视频播放器" section).
#
# Also checks the one exception: quitting while in app-fullscreen (F11) must NOT poison the
# memory with the borderless fullscreen rect (next launch would then be a fullscreen-sized
# window with a title bar).
#
# Why a temp copy of the exe: the app writes JarkViewer.db (settings) next to the exe, so
# running this against the repo build would clobber the user's own window size/settings.
#
# Usage:
#   pwsh tools/test_placement_memory.ps1 -Exe x64/Release/JarkViewer.exe -Media <video.mp4> -Image <img.png>
#
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param(
    [string]$Exe = "x64/Release/JarkViewer.exe",
    [Parameter(Mandatory = $true)][string]$Media,
    [Parameter(Mandatory = $true)][string]$Image,
    [string]$WorkDir = "$env:TEMP\jv-placement-test"
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System; using System.Runtime.InteropServices;
public class JvPlacement {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool flag);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public struct RECT { public int Left, Top, Right, Bottom; }

    // SetForegroundWindow only works for a process that already owns the foreground;
    // attaching the input queues first makes it reliable from a background script (same
    // trick as tools/capture_window.ps1 - without it the injected F11 lands in the terminal).
    public static bool ForceForeground(IntPtr hwnd) {
        if (GetForegroundWindow() == hwnd) return true;
        uint targetThread = GetWindowThreadProcessId(hwnd, out uint _p);
        uint currentThread = GetCurrentThreadId();
        IntPtr foreground = GetForegroundWindow();
        bool attachedForeground = false, attachedTarget = false;
        if (foreground != IntPtr.Zero) {
            uint fgThread = GetWindowThreadProcessId(foreground, out uint _f);
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

    // Largest visible top-level window of a pid. Enumerating in C# keeps the callback off
    // the PowerShell delegate bridge (and Process.MainWindowHandle caches the first handle
    // it ever saw, which is wrong for a process that swaps its window).
    public static IntPtr MainWindow(int pid) {
        IntPtr best = IntPtr.Zero; long bestArea = 0;
        EnumWindows((hwnd, l) => {
            if (!IsWindowVisible(hwnd)) return true;
            if (GetParent(hwnd) != IntPtr.Zero) return true;
            uint owner; GetWindowThreadProcessId(hwnd, out owner);
            if (owner != (uint)pid) return true;
            RECT r; if (!GetWindowRect(hwnd, out r)) return true;
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; best = hwnd; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    public static string Describe(IntPtr h) {
        RECT r; if (!GetWindowRect(h, out r)) return "(no rect)";
        return string.Format("{0},{1} {2}x{3}", r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top);
    }

    public static bool SameAs(IntPtr h, int x, int y, int w, int hh) {
        RECT r; if (!GetWindowRect(h, out r)) return false;
        return Math.Abs(r.Left - x) <= 4 && Math.Abs(r.Top - y) <= 4 &&
               Math.Abs((r.Right - r.Left) - w) <= 4 && Math.Abs((r.Bottom - r.Top) - hh) <= 4;
    }
}
"@

[void][JvPlacement]::SetProcessDPIAware()

$WM_CLOSE = 0x0010
$SW_RESTORE = 9
$VK_F11 = 0x7A
$KEYEVENTF_KEYUP = 0x0002
$script:failed = $false

function Open-App([string]$argument) {
    $proc = Start-Process -FilePath $script:exePath -ArgumentList @($argument) -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $proc.Refresh()
        if ($proc.HasExited) { throw "process exited early with code $($proc.ExitCode)" }
        $hwnd = [JvPlacement]::MainWindow($proc.Id)
        if ($hwnd -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 800   # let it settle (device/swapchain + first paint)
            return @{ proc = $proc; hwnd = $hwnd }
        }
    }
    throw "main window not found within 20s"
}

# Put the window at a known geometry (fresh settings open maximized, so restore first)
function Set-Geometry($app, [int]$x, [int]$y, [int]$w, [int]$h) {
    [void][JvPlacement]::ShowWindow($app.hwnd, $SW_RESTORE)
    Start-Sleep -Milliseconds 300
    [void][JvPlacement]::MoveWindow($app.hwnd, $x, $y, $w, $h, $true)
    Start-Sleep -Milliseconds 700
}

function Close-App($app) {
    [void][JvPlacement]::PostMessage($app.hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $app.proc.WaitForExit(15000)) {
        $app.proc.Kill()
        throw "process did not exit within 15s after WM_CLOSE"
    }
}

function Assert-Geometry([string]$name, $app, [int]$x, [int]$y, [int]$w, [int]$h) {
    if ([JvPlacement]::SameAs($app.hwnd, $x, $y, $w, $h)) {
        Write-Output ("PASS {0}: expected {1},{2} {3}x{4}, got {5}" -f $name, $x, $y, $w, $h, [JvPlacement]::Describe($app.hwnd))
    }
    else {
        Write-Output ("FAIL {0}: expected {1},{2} {3}x{4}, got {5}" -f $name, $x, $y, $w, $h, [JvPlacement]::Describe($app.hwnd))
        $script:failed = $true
    }
}

# --- isolated copy (the settings file lands next to the exe) ---
$source = (Resolve-Path -LiteralPath $Exe).Path
if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
[void](New-Item -ItemType Directory -Path $WorkDir)
$script:exePath = Join-Path $WorkDir (Split-Path -Leaf $source)
Copy-Item -LiteralPath $source -Destination $script:exePath
$settingFile = Join-Path $WorkDir "JarkViewer.db"
Write-Output "work dir: $WorkDir (isolated settings, the user's own file is untouched)"

$mediaPath = (Resolve-Path -LiteralPath $Media).Path
$imagePath = (Resolve-Path -LiteralPath $Image).Path

# 1) player: a distinctive geometry, then quit -> it must write the shared memory
$ax = 220; $ay = 170; $aw = 960; $ah = 660
$app = Open-App $mediaPath
Set-Geometry $app $ax $ay $aw $ah
Close-App $app
if (-not (Test-Path -LiteralPath $settingFile)) { throw "settings file was not written on exit: $settingFile" }

# 2) image viewer inherits it
$app = Open-App $imagePath
Start-Sleep -Milliseconds 400
Assert-Geometry "image viewer inherits the player's geometry" $app $ax $ay $aw $ah

# 3) image viewer: another geometry, then quit
$bx = 320; $by = 240; $bw = 1040; $bh = 700
Set-Geometry $app $bx $by $bw $bh
Close-App $app

# 4) player inherits it
$app = Open-App $mediaPath
Start-Sleep -Milliseconds 400
Assert-Geometry "player inherits the image viewer's geometry" $app $bx $by $bw $bh
Close-App $app

# 5) quitting while fullscreen (F11) must keep the last windowed geometry
$app = Open-App $mediaPath
Set-Geometry $app $ax $ay $aw $ah
if (-not [JvPlacement]::ForceForeground($app.hwnd)) {
    Close-App $app
    Write-Output "SKIP fullscreen case (could not take the foreground for key injection)"
}
else {
    Start-Sleep -Milliseconds 400
    [JvPlacement]::keybd_event($VK_F11, 0, 0, [IntPtr]::Zero)
    [JvPlacement]::keybd_event($VK_F11, 0, $KEYEVENTF_KEYUP, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800

    if ([JvPlacement]::SameAs($app.hwnd, $ax, $ay, $aw, $ah)) {
        Close-App $app
        Write-Output "SKIP fullscreen case (F11 did not change the geometry)"
    }
    else {
        Write-Output "      fullscreen geometry is $([JvPlacement]::Describe($app.hwnd))"
        Close-App $app                       # quit while fullscreen
        $app = Open-App $imagePath
        Start-Sleep -Milliseconds 400
        Assert-Geometry "quitting fullscreen keeps the last windowed geometry" $app $ax $ay $aw $ah
        Close-App $app
    }
}

# 6) the maximized state is shared too (showCmd), not just the windowed rect
$app = Open-App $mediaPath
Set-Geometry $app $ax $ay $aw $ah
[void][JvPlacement]::ShowWindow($app.hwnd, 3)   # SW_MAXIMIZE
Start-Sleep -Milliseconds 700
Close-App $app

$app = Open-App $imagePath
Start-Sleep -Milliseconds 400
if ([JvPlacement]::IsZoomed($app.hwnd)) {
    Write-Output "PASS maximized state is inherited (image viewer opened maximized)"
}
else {
    Write-Output "FAIL maximized state is inherited (image viewer opened windowed)"
    $script:failed = $true
}
Close-App $app

if ($script:failed) {
    Write-Output "placement memory: FAILED"
    exit 1
}
Write-Output "placement memory: all checks passed"
exit 0
