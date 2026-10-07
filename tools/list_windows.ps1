# Enumerate the visible top-level windows of a process with their rects.
# Usage: pwsh tools/list_windows.ps1 -ProcessId 1234
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param([Parameter(Mandatory = $true)][int]$ProcessId)

Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public class WinList {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int count);
    public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static List<string> Describe(int pid) {
        var list = new List<string>();
        EnumWindows(delegate(IntPtr hwnd, IntPtr param) {
            uint owner; GetWindowThreadProcessId(hwnd, out owner);
            if (owner == (uint)pid && IsWindowVisible(hwnd)) {
                RECT r; GetWindowRect(hwnd, out r);
                var sb = new StringBuilder(300);
                GetWindowTextW(hwnd, sb, 300);
                list.Add(string.Format("{0}x{1} at ({2},{3}) hwnd={4} title={5}",
                    r.Right - r.Left, r.Bottom - r.Top, r.Left, r.Top, hwnd, sb.ToString()));
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@

[WinList]::Describe($ProcessId) | ForEach-Object { $_ }
