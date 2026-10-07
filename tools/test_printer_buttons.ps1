# Printer window button check: open it with Ctrl+P, click 另存为 / 打印 hot areas and
# report the process windows after each click (a modal save dialog shows up as a new window).
#
# Usage: pwsh tools/test_printer_buttons.ps1 -Exe x64/Release/JarkViewer.exe -Image img.png
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Image
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public class JarkPrinter {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
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

    public static List<string> DescribeAll(int pid) {
        var list = new List<string>();
        EnumWindows(delegate(IntPtr hwnd, IntPtr param) {
            uint p; GetWindowThreadProcessId(hwnd, out p);
            if (p == (uint)pid) list.Add(Describe(hwnd));
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static string Describe(IntPtr hwnd) {
        RECT r; GetWindowRect(hwnd, out r);
        var sb = new StringBuilder(300);
        GetWindowTextW(hwnd, sb, 300);
        var cls = new StringBuilder(300);
        GetClassNameW(hwnd, cls, 300);
        return string.Format("{0}x{1} visible={2} class={3} title='{4}'",
            r.Right - r.Left, r.Bottom - r.Top, IsWindowVisible(hwnd), cls.ToString(), sb.ToString());
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, StringBuilder text, int count);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int count);

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
        System.Threading.Thread.Sleep(250);
        mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(100);
        mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }
}
"@

# Without this the host reports virtualized coordinates while SetCursorPos takes physical
# pixels, so every click lands somewhere else (this machine is 200% DPI).
Add-Type @"
using System; using System.Runtime.InteropServices;
public class JarkDpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[void][JarkDpi]::SetProcessDPIAware()

$proc = Start-Process -FilePath $Exe -ArgumentList $Image -PassThru
try {
    Start-Sleep -Seconds 5
    $before = [JarkPrinter]::Windows($proc.Id)

    if (-not [JarkPrinter]::ForceForeground($proc.MainWindowHandle)) { throw "cannot focus main window" }
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait('^p')
    Start-Sleep -Seconds 3

    $printer = [IntPtr]::Zero
    foreach ($hwnd in [JarkPrinter]::Windows($proc.Id)) {
        if ($before -notcontains $hwnd) { $printer = $hwnd; break }
    }
    if ($printer -eq [IntPtr]::Zero) { throw "printer window did not open" }
    "foreground ok: " + [JarkPrinter]::ForceForeground($printer)
    Start-Sleep -Milliseconds 400
    "printer: " + [JarkPrinter]::Describe($printer)
    "foreground hwnd: " + [JarkPrinter]::GetForegroundWindow() + " printer hwnd: " + $printer
    $pt = New-Object JarkPrinter+POINT
    $pt.X = 1500; $pt.Y = 50
    [void][JarkPrinter]::ClientToScreen($printer, [ref]$pt)
    "click screen point: " + $pt.X + "," + $pt.Y
    $r = New-Object JarkPrinter+RECT
    [void][JarkPrinter]::GetWindowRect($printer, [ref]$r)
    "window rect: " + $r.Left + "," + $r.Top + " - " + $r.Right + "," + $r.Bottom

    "--- all windows before clicking ---"
    foreach ($line in [JarkPrinter]::DescribeAll($proc.Id)) { "  " + $line }

    # hot areas: logical {600,0,100,50} = save as, {700,0,100,50} = print (x2 for 200% DPI)
    [JarkPrinter]::Click($printer, 1300, 50)
    Start-Sleep -Seconds 4
    "--- all windows after clicking save-as ---"
    foreach ($line in [JarkPrinter]::DescribeAll($proc.Id)) { "  " + $line }
    "process alive: " + (-not $proc.HasExited)
}
finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
