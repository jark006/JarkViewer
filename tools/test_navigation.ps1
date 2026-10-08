# 主界面导航冒烟：动态计算 DPI/客户区坐标，拖鸟瞰、悬停预览带、点击换图、滚轮与隐藏。
# 使用至少 5 张带编号的同目录图片；截图供像素比较及人工查看，不改文件关联或系统缓存。
param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Image,
    [Parameter(Mandatory=$true)][string]$OutDirectory,
    [switch]$CheckSettings
)
$ErrorActionPreference = 'Stop'
if ($CheckSettings -and -not ([IO.Path]::GetFullPath($Exe).StartsWith([IO.Path]::GetFullPath($env:TEMP), [StringComparison]::OrdinalIgnoreCase))) {
    throw 'CheckSettings requires an isolated executable copy under TEMP'
}
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class NavigationCheck {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint data, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    public static string Title(IntPtr h) { var b = new StringBuilder(32768); GetWindowText(h,b,b.Capacity); return b.ToString(); }
    public static void Move(IntPtr h, int x, int y) { var p = new POINT {X=x,Y=y}; ClientToScreen(h, ref p); SetCursorPos(p.X,p.Y); }
    public static bool Focus(IntPtr h) {
        uint pid; uint current = GetCurrentThreadId();
        uint target = GetWindowThreadProcessId(h, out pid);
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        bool a = fg != current && AttachThreadInput(current,fg,true);
        bool b = target != current && AttachThreadInput(current,target,true);
        ShowWindow(h,3); BringWindowToTop(h); SetForegroundWindow(h);
        if(b) AttachThreadInput(current,target,false);
        if(a) AttachThreadInput(current,fg,false);
        return GetForegroundWindow() == h;
    }
}
"@
[void][NavigationCheck]::SetProcessDPIAware()
New-Item -ItemType Directory -Force $OutDirectory | Out-Null
$proc = Start-Process -FilePath $Exe -ArgumentList ('"'+$Image+'"') -PassThru
try {
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    $focused=$false
    do {
        Start-Sleep -Milliseconds 250
        $proc.Refresh()
        if($proc.HasExited){ throw "App exited before creating a window: $($proc.ExitCode)" }
        $hwnd=$proc.MainWindowHandle
        if($hwnd -ne [IntPtr]::Zero){ $focused=[NavigationCheck]::Focus($hwnd) }
    } while(-not $focused -and [DateTime]::UtcNow -lt $deadline)
    if(-not $focused){ throw 'Cannot focus app window' }
    Start-Sleep -Milliseconds 1000
    $client = New-Object NavigationCheck+RECT
    [void][NavigationCheck]::GetClientRect($hwnd,[ref]$client)
    $w=$client.Right; $h=$client.Bottom; $s=[NavigationCheck]::GetDpiForWindow($hwnd)/96.0
    function Move-To($x,$y) { [NavigationCheck]::Move($hwnd,[int]$x,[int]$y); Start-Sleep -Milliseconds 160 }
    function Click-At($x,$y) {
        Move-To $x $y
        [NavigationCheck]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
        [NavigationCheck]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
        Start-Sleep -Milliseconds 800
    }
    function Save-Shot($name) {
        $r=New-Object NavigationCheck+RECT
        [void][NavigationCheck]::GetWindowRect($hwnd,[ref]$r)
        $b=New-Object System.Drawing.Bitmap(($r.Right-$r.Left),($r.Bottom-$r.Top))
        $g=[System.Drawing.Graphics]::FromImage($b); $dc=$g.GetHdc()
        try { if(-not [NavigationCheck]::PrintWindow($hwnd,$dc,2)){ throw 'PrintWindow failed' } }
        finally { $g.ReleaseHdc($dc); $g.Dispose() }
        $path=Join-Path $OutDirectory ($name+'.png')
        $b.Save($path,[System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
        Write-Output "captured=$path title=$([NavigationCheck]::Title($hwnd))"
    }
    Move-To ($w/2) ($h/2)
    [System.Windows.Forms.SendKeys]::SendWait('{UP 7}')
    Start-Sleep -Milliseconds 900
    Save-Shot '01-zoom'
    $edge=if($w -ge 500*$s){50*$s}else{$w/4}
    $bandWidth=$w-2*$edge-10*$s; $left=($w-$bandWidth)/2
    $panelWidth=[Math]::Min(200*$s,$bandWidth)
    $panelX=$left+$bandWidth-$panelWidth; $panelY=$h-154*$s
    Move-To ($panelX+$panelWidth*.5) ($panelY+72*$s)
    [NavigationCheck]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    for($n=1;$n -le 8;$n++){ Move-To ($panelX+$panelWidth*(.5-.03*$n)) ($panelY+(72-3*$n)*$s) }
    [NavigationCheck]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 400
    Save-Shot '02-pan'
    Move-To ($panelX+$panelWidth*.5) ($panelY+72*$s)
    [NavigationCheck]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    Move-To -20 100
    [NavigationCheck]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
    Move-To ($w/2) ($h/2)
    Save-Shot '03-outside-release'
    Move-To ($w/2) ($h-8*$s)
    Start-Sleep -Seconds 3
    Save-Shot '04-strip'
    $capacity=[Math]::Clamp([int][Math]::Floor(($bandWidth-52*$s)/(100*$s)),1,64)
    $cellWidth=($bandWidth-52*$s)/$capacity
    $cellY=$h-61*$s
    $before=[NavigationCheck]::Title($hwnd)
    Click-At ($left+26*$s+$cellWidth*1.5) $cellY
    $after=[NavigationCheck]::Title($hwnd)
    if($before -eq $after){ throw 'Thumbnail click did not change image' }
    Save-Shot '05-selected'
    [NavigationCheck]::mouse_event(0x0800,0,0,4294967176,[UIntPtr]::Zero)
    Start-Sleep -Seconds 2
    if([NavigationCheck]::Title($hwnd) -ne $after){ throw 'Strip wheel leaked to the main image' }
    Save-Shot '06-scrolled'
    Move-To ($w/2) ($h/2)
    Start-Sleep -Milliseconds 400
    Save-Shot '07-hidden'
    [System.Windows.Forms.SendKeys]::SendWait('{F1}')
    Start-Sleep -Milliseconds 800
    Save-Shot '08-settings'
    if($CheckSettings) {
        # 此分支只在临时目录的独立程序副本运行，坐标基于简体中文常规页。
        $sx=($w-680*$s)/2; $sy=($h-640*$s)/2
        Click-At ($sx+220*$s) ($sy+550*$s)
        Save-Shot '09-cache-cleared'
        # 清理在缓存线程异步完成：空索引 = 64 字节文件头 + 1000*64 字节索引槽
        $cache=Join-Path (Split-Path $Exe) 'JarkViewer.thumbnail'
        $cacheDeadline=[DateTime]::UtcNow.AddSeconds(5)
        while((Get-Item $cache).Length -ne 64064 -and [DateTime]::UtcNow -lt $cacheDeadline){ Start-Sleep -Milliseconds 200 }
        if((Get-Item $cache).Length -ne 64064){ throw "Cache clear did not leave an empty index: $((Get-Item $cache).Length)" }
        Click-At ($sx+28*$s) ($sy+270*$s)
    }
    [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    Start-Sleep -Milliseconds 300
    if($CheckSettings){ Save-Shot '10-navigator-disabled' }
    Click-At ($w/2) ($h/2)
    Write-Output 'PASS: navigator drag, outside release, hover strip, direct selection, wheel isolation, hide, settings round trip'
}
finally {
    if(-not $proc.HasExited) {
        [void]$proc.CloseMainWindow()
        if(-not $proc.WaitForExit(4000)){ $proc.Kill() }
    }
}
if($CheckSettings) {
    $settings=Join-Path (Split-Path $Exe) 'JarkViewer.db'
    $bytes=[IO.File]::ReadAllBytes($settings)
    if($bytes.Length -ne 4096 -or $bytes[67] -ne 1){ throw 'Navigator preference was not persisted' }
    $proc=Start-Process -FilePath $Exe -ArgumentList ('"'+$Image+'"') -PassThru
    try {
        Start-Sleep -Seconds 3
        $hwnd=$proc.MainWindowHandle
        if(-not [NavigationCheck]::Focus($hwnd)){ throw 'Cannot focus restarted app' }
        Start-Sleep -Milliseconds 400
        Save-Shot '11-disabled-after-restart'
        Write-Output 'PASS: cache cleared and navigator setting persisted across restart'
    }
    finally {
        if(-not $proc.HasExited) {
            [void]$proc.CloseMainWindow()
            if(-not $proc.WaitForExit(4000)){ $proc.Kill() }
        }
    }
}
