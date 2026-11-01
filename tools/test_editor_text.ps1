# Editor text-tool check: open the editor (Ctrl+E), pick the text tool, click an anchor on the
# canvas, click the sidebar text box, inject two CJK characters as Unicode WM_CHAR (which is what
# the IME delivers once a composition is committed) and capture the window. Verifies both the
# in-image anchor feedback (caret + live preview) and that CJK text reaches the ImGui input.
#
# Usage: pwsh tools/test_editor_text.ps1 -Exe x64/Release/JarkViewer.exe -Image img.png -Out shot.png
# 只支持 PowerShell 7（pwsh）：下面的守卫会拒绝 Windows PowerShell 5.1，并提示怎么装 pwsh。
# 本文件必须保留 UTF-8 BOM——5.1 会把无 BOM 的 .ps1 按 ANSI 解码，脚本在跑到守卫之前就已经
# 乱码/语法报错，用户看到的是一句莫名其妙的报错，而不是这条提示。

#       Coordinates below are physical client coordinates; retune them if the editor layout changes.

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$TextToolX = -1,    # -1: 按 DPI 与居中的编辑器位置计算
    [int]$TextToolY = -1,
    [int]$AnchorX = -1,
    [int]$AnchorY = -1,
    [int]$TextBoxX = -1,
    [int]$TextBoxY = -1
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

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public class JarkText {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int height, bool repaint);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, WM_CHAR = 0x0102;
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    // SetForegroundWindow only works for the foreground process; attaching the input queues first
    // makes it reliable from a background script (same trick as tools/capture_window.ps1).
    public static bool ForceForeground(IntPtr hwnd) {
        if (GetForegroundWindow() == hwnd) return true;
        uint target = GetWindowThreadProcessId(hwnd, out uint _p);
        uint current = GetCurrentThreadId();
        IntPtr fg = GetForegroundWindow();
        bool af = false, at = false;
        if (fg != IntPtr.Zero) {
            uint fgt = GetWindowThreadProcessId(fg, out uint _f);
            if (fgt != current) af = AttachThreadInput(current, fgt, true);
        }
        if (target != current) at = AttachThreadInput(current, target, true);
        ShowWindow(hwnd, 9); BringWindowToTop(hwnd);
        bool ok = SetForegroundWindow(hwnd);
        if (at) AttachThreadInput(current, target, false);
        if (af) AttachThreadInput(current, GetWindowThreadProcessId(fg, out uint _f2), false);
        return ok || GetForegroundWindow() == hwnd;
    }

    public static void Click(IntPtr hwnd, int x, int y) {
        POINT p = new POINT(); p.X = x; p.Y = y;
        ClientToScreen(hwnd, ref p);
        SetCursorPos(p.X, p.Y);
        System.Threading.Thread.Sleep(150);
        mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(70);
        mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }
}
"@

[void][JarkText]::SetProcessDPIAware()

$ErrorActionPreference = 'Stop'
$proc = Start-Process -FilePath $Exe -ArgumentList ('"'+$Image+'"') -PassThru
try {
    Start-Sleep -Seconds 4
    $hwnd = $proc.MainWindowHandle
    if ($hwnd -eq [IntPtr]::Zero) { throw "main window not found" }
    if (-not [JarkText]::ForceForeground($hwnd)) { throw "cannot focus the main window" }
    $scale=[JarkText]::GetDpiForWindow($hwnd)/96.0
    if($TextToolX -lt 0) {
        # 固定足够容纳单行工具栏的视口，避免旧脚本的 4K 坐标在 1080p 上点到窗口之外仍报成功。
        $work=[System.Windows.Forms.Screen]::FromHandle($hwnd).WorkingArea
        [void][JarkText]::MoveWindow($hwnd,$work.X,$work.Y,
            [Math]::Min($work.Width,[int](1280*$scale)),[Math]::Min($work.Height,[int](900*$scale)),$true)
        Start-Sleep -Milliseconds 500
        $client=New-Object JarkText+RECT
        [void][JarkText]::GetClientRect($hwnd,[ref]$client)
        $editorWidth=[Math]::Min(1100*$scale,$client.Right-8*$scale)
        $editorHeight=[Math]::Min(760*$scale,$client.Bottom-8*$scale)
        if($editorWidth -lt 1050*$scale){ throw 'Text test requires room for a single-row editor toolbar' }
        $left=($client.Right-$editorWidth)/2; $top=($client.Bottom-$editorHeight)/2
        $TextToolX=[int]($left+816*$scale); $TextToolY=[int]($top+55*$scale)
        $AnchorX=[int]($left+250*$scale); $AnchorY=[int]($top+300*$scale)
        $TextBoxX=[int]($left+$editorWidth-150*$scale); $TextBoxY=[int]($top+330*$scale)
    }
    Start-Sleep -Milliseconds 400

    # 编辑器是主窗口里的 ImGui 窗口（不再是独立窗口），所以直接对主窗口点按即可
    [System.Windows.Forms.SendKeys]::SendWait('^e')
    Start-Sleep -Seconds 2

    [JarkText]::Click($hwnd, $TextToolX, $TextToolY)   # text tool
    Start-Sleep -Milliseconds 400
    [JarkText]::Click($hwnd, $AnchorX, $AnchorY)       # anchor on the canvas
    Start-Sleep -Milliseconds 400
    [JarkText]::Click($hwnd, $TextBoxX, $TextBoxY)     # sidebar text box
    Start-Sleep -Milliseconds 600

    function Capture-Editor {
        $r=New-Object JarkText+RECT
        [void][JarkText]::GetWindowRect($hwnd,[ref]$r)
        $bitmap=New-Object System.Drawing.Bitmap(($r.Right-$r.Left),($r.Bottom-$r.Top))
        $graphics=[System.Drawing.Graphics]::FromImage($bitmap); $dc=$graphics.GetHdc()
        try { if(-not [JarkText]::PrintWindow($hwnd,$dc,2)){ throw 'PrintWindow failed' } }
        finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
        return ,$bitmap
    }
    $before=Capture-Editor

    # Unicode WM_CHAR: what the window receives after the IME commits a composition
    foreach ($code in @(0x6D4B, 0x8BD5)) {   # 测 试
        [void][JarkText]::PostMessageW($hwnd, [JarkText]::WM_CHAR, [IntPtr]$code, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 300
    }
    Start-Sleep -Milliseconds 800

    $rect = New-Object JarkText+RECT
    [void][JarkText]::GetWindowRect($hwnd, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    [void][JarkText]::PrintWindow($hwnd, $hdc, 2)   # PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $changed=0; $red=0
    for($y=0;$y -lt $height;$y++) {
        for($x=0;$x -lt $width;$x++) {
            $old=$before.GetPixel($x,$y); $new=$bitmap.GetPixel($x,$y)
            if([Math]::Abs([int]$old.R-$new.R)+[Math]::Abs([int]$old.G-$new.G)+[Math]::Abs([int]$old.B-$new.B) -gt 60) {
                $changed++
                if($new.R -gt 180 -and $new.G -lt 130 -and $new.B -lt 130){ $red++ }
            }
        }
    }
    $before.Dispose()
    $bitmap.Dispose()
    if($changed -lt 40 -or $red -lt 15){ throw "CJK input/live preview missing: changed=$changed red=$red screenshot=$Out" }
    "PASS: CJK input and red canvas preview changed=$changed red=$red size=${width}x${height} out=$Out"
}
finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
