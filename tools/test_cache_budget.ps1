# 图像缓存的实机回归：连续翻页时工作集必须收敛在缓存预算内。
#
# 预算的口径（见 JarkViewer/include/ImageAssetCache.h、ImageDatabase::defaultCacheBudgetBytes）：
#   * 条数最多 10 张、字节最多物理内存的 50%，两个上限先到先算；
#   * 至少留住 2 张（翻回上一张不用重解码）；
#   * 当前正在显示的那张由主窗口持有，不参与淘汰。
# 这条回归存在的意义：按张数留缓存时，一台 32GB 机器连翻 4 张 43890x38875 的扫描件
# 就是 25GB（每张解码后 6.36GB），直接爆内存。
#
# Usage:
#   pwsh tools/test_cache_budget.ps1 -Exe x64/Release/JarkViewer.exe -Image "D:\test\a.jxl"
#   pwsh tools/test_cache_budget.ps1 -Exe ... -Image a.jxl -Copies 6 -KeepRunning
#
# -Image 拿来做 N 份副本（挑一张解码快、尺寸中等的）；程序与语料都在 %TEMP% 的独立
# 目录里跑，设置/缓存文件写在 exe 旁边，不会动到用户自己的 JarkViewer.db。
#
# 只支持 PowerShell 7（pwsh）：下面的守卫会拒绝 Windows PowerShell 5.1，并提示怎么装 pwsh。
# 本文件必须保留 UTF-8 BOM——5.1 会把无 BOM 的 .ps1 按 ANSI 解码，脚本在跑到守卫之前就已经
# 乱码/语法报错，用户看到的是一句莫名其妙的报错，而不是这条提示。

param(
    [string]$Exe = "x64/Release/JarkViewer.exe",
    [Parameter(Mandatory = $true)][string]$Image,
    [int]$Copies = 12,
    [int]$ExpectMaxEntries = 10,
    [int]$MarginMB = 900,
    [int]$StartupMs = 8000,
    [int]$StepMs = 2500,
    [switch]$KeepRunning,
    [string]$OutDirectory = "$env:TEMP\JarkViewer-cache-budget"
)

if ($PSVersionTable.PSEdition -ne 'Core') {
    Write-Host "This script requires PowerShell 7 (pwsh); Windows PowerShell $($PSVersionTable.PSVersion) is not supported." -ForegroundColor Red
    Write-Host "Re-run with:  pwsh -File `"$PSCommandPath`"" -ForegroundColor Yellow
    Write-Host "Install:      winget install --id Microsoft.PowerShell" -ForegroundColor Yellow
    exit 1
}

Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class JarkCacheTest {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr hWnd, IntPtr l);
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h)) {
                StringBuilder sb = new StringBuilder(512);
                GetWindowTextW(h, sb, 512);
                if (sb.Length > 0) { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static void Key(IntPtr hwnd, int vk) {
        PostMessage(hwnd, 0x0100, (IntPtr)vk, (IntPtr)0x004D0001);
        // 0xC04D0001 超过 int 范围，C# 里必须 unchecked，否则 Add-Type 直接编译失败
        PostMessage(hwnd, 0x0101, (IntPtr)vk, unchecked((IntPtr)(long)0xC04D0001L));
    }
}
"@

$VK_LEFT = 0x25   # 上一张
$VK_RIGHT = 0x27  # 下一张

# 任何一处出错都要当场炸掉：C# 辅助类没编译出来时后面会一路"取不到类型"，
# 采样全是同一个数，反倒会被后面的断言当成"工作集很稳"而误报通过。
$ErrorActionPreference = 'Stop'

$failures = 0
function Check([bool]$ok, [string]$name) {
    if ($ok) { Write-Host "  [ok]   $name" }
    else { Write-Host "  [FAIL] $name" -ForegroundColor Red; $script:failures++ }
    return $ok
}

# —— 准备独立运行环境（exe 副本 + N 份语料）——
$source = (Resolve-Path -LiteralPath $Image).Path
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$exeDir = Join-Path $OutDirectory "app"
$dataDir = Join-Path $OutDirectory "images"
Remove-Item -LiteralPath $OutDirectory -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $exeDir, $dataDir | Out-Null
Copy-Item -LiteralPath $exePath -Destination $exeDir
Get-ChildItem -LiteralPath (Split-Path $exePath) -Filter *.dll -ErrorAction SilentlyContinue |
    Copy-Item -Destination $exeDir -ErrorAction SilentlyContinue

$ext = [System.IO.Path]::GetExtension($source)
for ($i = 1; $i -le $Copies; $i++) {
    Copy-Item -LiteralPath $source -Destination (Join-Path $dataDir ("img{0:d2}{1}" -f $i, $ext))
}

$physicalMB = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB)
$budgetMB = $physicalMB / 2
Write-Host "语料：$Copies 份 $(Split-Path $source -Leaf)（各约 $([math]::Round((Get-Item $source).Length / 1MB)) MB）"
Write-Host "物理内存 $physicalMB MB -> 字节预算 $([math]::Round($budgetMB)) MB，条数上限 $ExpectMaxEntries"
Write-Host "运行目录：$OutDirectory"

# —— 起程序并采样 ——
$first = Join-Path $dataDir ("img01" + $ext)
$proc = Start-Process -FilePath (Join-Path $exeDir "JarkViewer.exe") -ArgumentList (Split-Path $first -Leaf) `
    -WorkingDirectory $dataDir -PassThru
Start-Sleep -Milliseconds $StartupMs
if ($proc.HasExited) { Write-Host "程序启动即退出：$($proc.ExitCode)" -ForegroundColor Red; exit 1 }

$hwnd = [JarkCacheTest]::Find([uint32]$proc.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "没找到窗口" -ForegroundColor Red; $proc.Kill(); exit 1 }

$samples = @()
function Sample([string]$tag) {
    $proc.Refresh()
    $mb = [math]::Round($proc.WorkingSet64 / 1MB)
    $script:samples += [pscustomobject]@{ Tag = $tag; MB = $mb }
    Write-Host ("  {0,-14} 工作集 {1,8:N0} MB" -f $tag, $mb)
    return $mb
}

$baseline = Sample "打开第 1 张"
$oneMB = 0
for ($i = 2; $i -le $Copies; $i++) {
    [JarkCacheTest]::Key($hwnd, $VK_RIGHT)
    Start-Sleep -Milliseconds $StepMs
    $mb = Sample "翻到第 $i 张"
    if ($i -eq 2) { $oneMB = $mb - $baseline }   # 一张图的解码字节（自标定）
}
$peak = ($samples | Measure-Object -Property MB -Maximum).Maximum

# 翻回上一张：至少 2 张还在缓存里，工作集不该再涨
[JarkCacheTest]::Key($hwnd, $VK_LEFT)
Start-Sleep -Milliseconds $StepMs
$back = Sample "翻回上一张"

Write-Host ""
Write-Host "每张约 $oneMB MB；峰值 $peak MB"

# ⓪ 前提：按键确实换过图（换图后工作集要涨一张的量）。没涨就说明按键没进去、
#    或者语料选得太小，后面的"稳不稳"全都不可信——先报 FAIL 而不是当成通过。
if (-not (Check ($oneMB -gt 8) "翻页生效（第 2 张比第 1 张多出 $oneMB MB）")) {
    if (-not $KeepRunning) { $proc.Kill() }
    Write-Host "图像缓存预算回归：环境没准备好，后续断言不成立" -ForegroundColor Red
    exit 1
}

# ① 至少 2 张：翻回上一张不重新解码
$null = Check ($back -le $peak + 16) "翻回上一张命中的是缓存（工作集 $back MB 未超过峰值 $peak MB）"

# ② 上限：不超过 min(内存一半, 10 张) + 程序自身基线 + 余量
$expectedCapMB = [math]::Min($budgetMB, $ExpectMaxEntries * $oneMB)
$appBaseline = $baseline - $oneMB     # 第 1 张之外的开销（D3D/字体/解码缓冲…）
$allowed = $appBaseline + $expectedCapMB + $MarginMB
$null = Check ($peak -le $allowed) "峰值在预算内（$peak MB <= $allowed MB，其中预算 $([math]::Round($expectedCapMB)) MB）"

# ③ 收敛：翻到后面不再逐张增长
$tail = $samples | Where-Object { $_.Tag -like "翻到第*" } | Select-Object -Last 3
$growth = ($tail | Measure-Object -Property MB -Maximum).Maximum - ($tail | Measure-Object -Property MB -Minimum).Minimum
$null = Check ($growth -le [math]::Max(32, $oneMB / 10)) "翻页后工作集收敛（最后三张波动 $growth MB）"

if (-not $KeepRunning) { $proc.Kill() } else { Write-Host "程序还在跑（-KeepRunning）：pid=$($proc.Id)" }

Write-Host ""
if ($failures -eq 0) { Write-Host "图像缓存预算回归：全部通过" -ForegroundColor Green; exit 0 }
Write-Host "图像缓存预算回归：$failures 项失败" -ForegroundColor Red
exit 1
