# 重命名顺序回归：Ctrl+R 改名后文件列表保持原位（逐张重命名不跳位），
# 并验证"重新打开目录"会恢复真实排序。全程不点坐标，只靠标题栏判读，布局改了也不用重量。
#
# 用法：pwsh tools/test_rename_order.ps1 [-Exe x64/Release/JarkViewer.exe] [-WorkDir <临时目录>]
# 只支持 PowerShell 7（pwsh）：下面的守卫会拒绝 Windows PowerShell 5.1，并提示怎么装 pwsh。
# 本文件必须保留 UTF-8 BOM——5.1 会把无 BOM 的 .ps1 按 ANSI 解码，脚本在跑到守卫之前就已经
# 乱码/语法报错，用户看到的是一句莫名其妙的报错，而不是这条提示。

param(
    [string]$Exe = "x64/Release/JarkViewer.exe",
    [string]$WorkDir = ""
)

# --- PowerShell 7 (pwsh) only -------------------------------------------------------------
# Windows PowerShell 5.1 is refused below: it reads BOM-less .ps1 files as ANSI (mojibake, and
# sometimes a syntax error that hides this guard) and quotes Start-Process arguments
# differently, which the UI test scripts depend on. Run everything with pwsh.
if ($PSVersionTable.PSEdition -ne 'Core') {
    Write-Host "This script requires PowerShell 7 (pwsh); Windows PowerShell $($PSVersionTable.PSVersion) is not supported." -ForegroundColor Red
    Write-Host ('Re-run with:  pwsh -File "' + $PSCommandPath + '"') -ForegroundColor Yellow
    Write-Host "Install:      winget install --id Microsoft.PowerShell" -ForegroundColor Yellow
    exit 1
}

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }
if (-not (Test-Path $Exe)) { Write-Host "找不到程序：$Exe" -ForegroundColor Red; exit 1 }
if ($WorkDir -eq "") { $WorkDir = Join-Path ([IO.Path]::GetTempPath()) ("JarkViewer-rename-order-" + [guid]::NewGuid().ToString("N").Substring(0, 8)) }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class JarkRename {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public const uint WM_CHAR = 0x0102, KEYUP = 0x0002;

    // 后台脚本拿不到前台权限，先附加输入队列再 SetForegroundWindow（与 capture_window 同一套做法）
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

    // Ctrl+R 必须走真实按键：组合键不会被 TranslateMessage 变成字符（投递 WM_KEYDOWN 会冒出 'r'）
    public static void CtrlR() {
        keybd_event(0x11, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(70);
        keybd_event(0x52, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(70);
        keybd_event(0x52, 0, KEYUP, UIntPtr.Zero); System.Threading.Thread.Sleep(70);
        keybd_event(0x11, 0, KEYUP, UIntPtr.Zero);
    }
    public static void PressKey(byte vk) {
        keybd_event(vk, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(70);
        keybd_event(vk, 0, KEYUP, UIntPtr.Zero);
    }
}
"@

$script:failed = 0
function Check([bool]$ok, [string]$name, [string]$detail) {
    if ($ok) { Write-Host "  [ok] $name  ($detail)" }
    else { Write-Host "  [FAIL] $name  ($detail)" -ForegroundColor Red; $script:failed++ }
}
function Get-Title([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][JarkRename]::GetWindowTextW($h, $sb, 512)
    return $sb.ToString()
}
function Wait-Title([IntPtr]$h, [string]$pattern, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        $title = Get-Title $h
        if ($title -match $pattern) { return $title }
        Start-Sleep -Milliseconds 150
    } while ((Get-Date) -lt $deadline)
    return (Get-Title $h)
}
function Send-Char([IntPtr]$h, [char]$c) {
    [void][JarkRename]::PostMessageW($h, [JarkRename]::WM_CHAR, [IntPtr][int]$c, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 140
}
# 改名一步：Ctrl+R 开窗 → Backspace 删掉预填名（输入框是 AutoSelectAll）→ 逐字 WM_CHAR → Enter
function Rename-To([IntPtr]$h, [string]$name) {
    [void][JarkRename]::ForceForeground($h); Start-Sleep -Milliseconds 300
    [JarkRename]::CtrlR(); Start-Sleep -Milliseconds 1200
    for ($i = 0; $i -lt 8; $i++) { [JarkRename]::PressKey(0x08) }
    Start-Sleep -Milliseconds 300
    foreach ($ch in $name.ToCharArray()) { Send-Char $h $ch }
    Start-Sleep -Milliseconds 400
    [JarkRename]::PressKey(0x0D)
    $title = Wait-Title $h ([regex]::Escape($name)) 10
    # 关窗后补按一次 Ctrl 清状态：Ctrl 抬起曾被 ImGui 吃掉，否则下一次按 → 会变成 Ctrl+→ 被吞
    [void][JarkRename]::ForceForeground($h); Start-Sleep -Milliseconds 300
    [JarkRename]::PressKey(0x11); Start-Sleep -Milliseconds 300
    return $title
}
function Start-Viewer([string]$argument) {
    $proc = Start-Process -FilePath $Exe -ArgumentList $argument -WorkingDirectory $WorkDir -PassThru
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250; $proc.Refresh(); $hwnd = $proc.MainWindowHandle
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "主窗口没起来" }
    [void][JarkRename]::ForceForeground($hwnd); Start-Sleep -Milliseconds 900
    return @{ Process = $proc; Window = $hwnd }
}

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Copy-Item $Exe $WorkDir
for ($i = 1; $i -le 5; $i++) {
    $bitmap = New-Object System.Drawing.Bitmap 64, 48
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::FromArgb(25 * $i, 70, 210 - 30 * $i))
    $graphics.Dispose()
    $bitmap.Save((Join-Path $WorkDir ("img{0}.png" -f $i)), [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}

Write-Host "== 重命名顺序回归 =="
try {
    $viewer = Start-Viewer "img3.png"
    try {
        $title = Wait-Title $viewer.Window '^\[3/5\] img3\.png' 8
        Check ($title -match '^\[3/5\] img3\.png') "打开 img3 时序号正确" $title

        $title = Rename-To $viewer.Window "zzz"
        Check ($title -match '^\[3/5\] zzz\.png') "改名后仍在第 3 位（不重排）" $title
        Check (Test-Path (Join-Path $WorkDir "zzz.png")) "磁盘上出现 zzz.png" "zzz.png"
        Check (-not (Test-Path (Join-Path $WorkDir "img3.png"))) "磁盘上 img3.png 已消失" "img3.png"

        [JarkRename]::PressKey(0x27)   # →
        $title = Wait-Title $viewer.Window '^\[4/5\]' 8
        Check ($title -match '^\[4/5\] img4\.png') "改名后按 → 仍是 img4（邻居没变）" $title

        $title = Rename-To $viewer.Window "qqq"
        Check ($title -match '^\[4/5\] qqq\.png') "第二次改名仍在第 4 位" $title
        [JarkRename]::PressKey(0x27)
        $title = Wait-Title $viewer.Window '^\[5/5\]' 8
        Check ($title -match '^\[5/5\] img5\.png') "第二次改名后按 → 仍是 img5" $title
    }
    finally {
        if (-not $viewer.Process.HasExited) { $viewer.Process.Kill(); [void]$viewer.Process.WaitForExit(3000) }
    }

    # 重新打开目录 = 恢复真实排序：img1 img2 img5 qqq zzz（自然序），qqq 落在第 4 位
    $viewer = Start-Viewer "qqq.png"
    try {
        $title = Wait-Title $viewer.Window '^\[4/5\]' 8
        Check ($title -match '^\[4/5\] qqq\.png') "重新打开目录后恢复真实排序（qqq 第 4 位）" $title
        [JarkRename]::PressKey(0x27)
        $title = Wait-Title $viewer.Window '^\[5/5\]' 8
        Check ($title -match '^\[5/5\] zzz\.png') "恢复排序后按 → 是 zzz" $title
    }
    finally {
        if (-not $viewer.Process.HasExited) { $viewer.Process.Kill(); [void]$viewer.Process.WaitForExit(3000) }
    }
}
finally {
    Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failed -gt 0) {
    Write-Host "重命名顺序回归存在失败项" -ForegroundColor Red
    exit 1
}
Write-Host "重命名顺序回归全部通过" -ForegroundColor Green
exit 0

