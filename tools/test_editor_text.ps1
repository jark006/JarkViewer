# Editor text tool check: pick the text tool, place an anchor, inject two CJK characters
# through WM_IME_CHAR (DBCS bytes of the system code page), press Enter, capture the window.
#
# Usage: pwsh tools/test_editor_text.ps1 -Exe x64/Release/JarkViewer.exe -Image img.png -Out shot.png
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$TextToolX = 2288,   # physical client coords of the text tool button
    [int]$TextToolY = 400,
    [int]$AnchorX = 600,      # physical client coords of the text anchor on the canvas
    [int]$AnchorY = 800
)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public class JarkText {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    public const uint WM_IME_CHAR = 0x0286, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101;
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public static List<IntPtr> Windows(int pid) {
        var list = new List<IntPtr>();
        EnumWindows(delegate(IntPtr hwnd, IntPtr param) {
            uint p; GetWindowThreadProcessId(hwnd, out p);
            if (p == (uint)pid && IsWindowVisible(hwnd)) list.Add(hwnd);
            return true;
        }, IntPtr.Zero);
        return list;
    }

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
        ShowWindow(hwnd, 9);
        BringWindowToTop(hwnd);
        bool ok = SetForegroundWindow(hwnd);
        if (attachedTarget) AttachThreadInput(currentThread, targetThread, false);
        if (attachedForeground) AttachThreadInput(currentThread, GetWindowThreadProcessId(foreground, out uint _f2), false);
        return ok || GetForegroundWindow() == hwnd;
    }

    public static void Click(IntPtr hwnd, int clientX, int clientY) {
        POINT pt = new POINT(); pt.X = clientX; pt.Y = clientY;
        ClientToScreen(hwnd, ref pt);
        SetCursorPos(pt.X, pt.Y);
        System.Threading.Thread.Sleep(200);
        mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }
}
"@

[void][JarkText]::SetProcessDPIAware()

$proc = Start-Process -FilePath $Exe -ArgumentList $Image -PassThru
try {
    Start-Sleep -Seconds 5
    $before = [JarkText]::Windows($proc.Id)

    if (-not [JarkText]::ForceForeground($proc.MainWindowHandle)) { throw "cannot focus main window" }
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait('^e')
    Start-Sleep -Seconds 3

    $editor = [IntPtr]::Zero
    foreach ($hwnd in [JarkText]::Windows($proc.Id)) {
        if ($before -notcontains $hwnd) { $editor = $hwnd; break }
    }
    if ($editor -eq [IntPtr]::Zero) { throw "editor window did not open" }
    if (-not [JarkText]::ForceForeground($editor)) { throw "cannot focus editor window" }
    Start-Sleep -Milliseconds 300

    [JarkText]::Click($editor, $TextToolX, $TextToolY)   # text tool
    Start-Sleep -Milliseconds 400
    [JarkText]::Click($editor, $AnchorX, $AnchorY)       # place the anchor
    Start-Sleep -Milliseconds 400

    # DBCS bytes for two simplified Chinese characters, packed as WM_IME_CHAR expects
    $chars = @(0xE2B2, 0xD4CA)   # 测 试 in code page 936
    foreach ($packed in $chars) {
        [void][JarkText]::PostMessageW($editor, [JarkText]::WM_IME_CHAR, [IntPtr]$packed, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 250
    }

    [void][JarkText]::PostMessageW($editor, [JarkText]::WM_KEYDOWN, [IntPtr]13, [IntPtr]::Zero)
    [void][JarkText]::PostMessageW($editor, [JarkText]::WM_KEYUP, [IntPtr]13, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800

    $rect = New-Object JarkText+RECT
    [void][JarkText]::GetWindowRect($editor, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top

    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    $printed = [JarkText]::PrintWindow($editor, $hdc, 2)
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()

    "captured=$printed size=${width}x${height} out=$Out"
}
finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
