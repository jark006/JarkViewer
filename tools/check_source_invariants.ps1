# 源码不变量检查：查一批"跑起来也看不出来、但一改就会悄悄坏"的约定。
# 只读源码，几秒钟出结果；提交前可单独跑。反向验证见 verify_source_invariant_checks.ps1
# （逐项制造错误确认真会报错——静态检查最危险的失效方式是写歪了正则照样 PASS）。
#
# 检查项：
#   1/2. 两张字符串表的 // N 编号与实际下标一致（编号被硬编码引用，中间插条目会整体错位）
#   3.   默认关联列表（defaultExtList）都在 supportExt/supportRaw 里
#   4/5. README 的静态/RAW 格式清单与代码里的集合完全一致
#   6.   PSD 解码顺序是 psd_sdk 优先、stb 兜底（反了 16 位 RLE 会全透明且不报错）
#   7.   ImageDatabase 析构里先停 LRU 预读线程（不然退出时在途解码访问已释放成员）
#   8.   右键菜单加速键不重复（重复时只有先出现的那个能按）
#   9.   main.cpp / .rc 字符串版 / .rc 数字版 版本号一致（升级时最容易漏改 .rc 数字）
param([string]$Root = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = "Stop"
$script:failed = $false

function Read-Source([string]$relative) {
    return Get-Content -Raw -LiteralPath (Join-Path $Root $relative) -Encoding UTF8
}

function Report([bool]$ok, [string]$name) {
    if ($ok) { Write-Host "  [ok] $name" }
    else { Write-Host "  [FAIL] $name" -ForegroundColor Red; $script:failed = $true }
}

Write-Host "== 源码不变量检查 =="

# 1/2. 字符串表编号
$stringRes = Read-Source "JarkViewer/src/stringRes.cpp"
foreach ($tableName in @("UIStringTable[", "UIStringTableWide[")) {
    $startIndex = $stringRes.IndexOf("std::string_view $tableName")
    $endIndex = $stringRes.IndexOf("`n};", $startIndex)
    if ($startIndex -lt 0 -or $endIndex -lt 0) {
        Report $false "$tableName 找不到表体"
        continue
    }
    $block = $stringRes.Substring($startIndex, $endIndex - $startIndex)
    $index = 0
    $mismatch = @()
    foreach ($line in ($block -split "`n")) {
        if ($line -match '^\s*\{') {
            if ($line -match '//\s*(\d+)') {
                if ([int]$Matches[1] -ne $index) { $mismatch += "第 $index 条标注为 $($Matches[1])" }
            }
            $index++
        }
    }
    Report ($mismatch.Count -eq 0) "$tableName 的 // N 编号与下标一致（共 $index 条）"
    $mismatch | Select-Object -First 5 | ForEach-Object { Write-Host "    $_" }
}

# 3. 默认关联列表
$jarkUtils = Read-Source "JarkViewer/include/jarkUtils.h"
$imageDbHeader = Read-Source "JarkViewer/include/ImageDatabase.h"

function Get-ExtSet([string]$text, [string]$marker) {
    $start = $text.IndexOf($marker)
    $end = $text.IndexOf("};", $start)
    $block = $text.Substring($start, $end - $start)
    $set = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($match in [regex]::Matches($block, 'L"([a-z0-9]+)"')) {
        [void]$set.Add($match.Groups[1].Value)
    }
    return $set
}

$supportExt = Get-ExtSet $imageDbHeader "supportExt{"
$supportRaw = Get-ExtSet $imageDbHeader "supportRaw{"
$defaultList = [regex]::Match($jarkUtils, 'defaultExtList\{\s*"([^"]+)"').Groups[1].Value -split ','
$missingDefaults = @($defaultList | Where-Object { -not $supportExt.Contains($_) -and -not $supportRaw.Contains($_) })
Report ($missingDefaults.Count -eq 0) "默认关联列表全部在 supportExt/supportRaw 里"
$missingDefaults | ForEach-Object { Write-Host "    缺失: $_" }

# 4/5. README 格式清单
$readme = Read-Source "README.md"
function Get-ReadmeList([string]$pattern) {
    $value = [regex]::Match($readme, $pattern).Groups[1].Value
    $set = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($item in ($value -split ' ')) { if ($item) { [void]$set.Add($item) } }
    return $set
}

$readmeStatic = Get-ReadmeList '- \*\*静态\*\*：`([^`]+)`'
$readmeRaw = Get-ReadmeList '- \*\*RAW\*\*：`([^`]+)`'

$staticDiff = @()
foreach ($ext in $supportExt) { if (-not $readmeStatic.Contains($ext)) { $staticDiff += "README 缺 $ext" } }
foreach ($ext in $readmeStatic) { if (-not $supportExt.Contains($ext)) { $staticDiff += "README 多 $ext" } }
Report ($staticDiff.Count -eq 0) "README 静态清单 == supportExt（$($supportExt.Count) 项）"
$staticDiff | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }

$rawDiff = @()
foreach ($ext in $supportRaw) { if (-not $readmeRaw.Contains($ext)) { $rawDiff += "README 缺 $ext" } }
foreach ($ext in $readmeRaw) { if (-not $supportRaw.Contains($ext)) { $rawDiff += "README 多 $ext" } }
Report ($rawDiff.Count -eq 0) "README RAW 清单 == supportRaw（$($supportRaw.Count) 项）"
$rawDiff | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }

# 6. PSD 解码顺序
$imageDbCpp = Read-Source "JarkViewer/src/ImageDatabase.cpp"
$psdCaseIndex = $imageDbCpp.IndexOf("case jark::FileFormat::Psd:")
if ($psdCaseIndex -ge 0) {
    $psdBlock = $imageDbCpp.Substring($psdCaseIndex, [Math]::Min(800, $imageDbCpp.Length - $psdCaseIndex))
    $psdFirst = $psdBlock.IndexOf("loadPSD(")
    $stbFallback = $psdBlock.IndexOf("loadSTB(")
    Report ($psdFirst -ge 0 -and $stbFallback -ge 0 -and $psdFirst -lt $stbFallback) `
        "PSD 解码顺序 psd_sdk 优先、stb 兜底"
}
else {
    Report $false "找不到 PSD 解码分支"
}

# 7. LRU 预读线程在派生类析构里先停
$dtorIndex = $imageDbHeader.IndexOf("~ImageDatabase()")
if ($dtorIndex -ge 0) {
    $dtorBlock = $imageDbHeader.Substring($dtorIndex, [Math]::Min(400, $imageDbHeader.Length - $dtorIndex))
    Report ($dtorBlock.Contains("stopPreloadWorker")) "ImageDatabase 析构先停预读线程"
}
else {
    Report $false "找不到 ~ImageDatabase"
}

# 8. 右键菜单加速键唯一（文案在宽表里：先取 CreateContextMenu 引用的 ID，再看这些文案的 (&X)）
$d3dCpp = Read-Source "JarkViewer/src/D3D11App.cpp"
$menuStart = $d3dCpp.IndexOf("HMENU hMenu = CreatePopupMenu();")
$menuEnd = $d3dCpp.IndexOf("return hMenu;", $menuStart)
$menuBlock = $d3dCpp.Substring($menuStart, $menuEnd - $menuStart)
$menuIds = @([regex]::Matches($menuBlock, 'getUIStringW\((\d+)\)') | ForEach-Object { [int]$_.Groups[1].Value })

$wideStart = $stringRes.IndexOf("std::string_view UIStringTableWide[")
$wideEnd = $stringRes.IndexOf("`n};", $wideStart)
$wideBlock = $stringRes.Substring($wideStart, $wideEnd - $wideStart)
$wideRows = @(($wideBlock -split "`n") | Where-Object { $_ -match '^\s*\{' })

$keys = @()
foreach ($id in $menuIds) {
    if ($id -lt $wideRows.Count -and $wideRows[$id] -match '\(&([A-Za-z])\)') {
        $keys += $Matches[1].ToUpperInvariant()
    }
}
# 老菜单自带的重复（A/E/P/S 各两项，按键时先出现的那项优先），属历史遗留；
# 这里只负责"不许新增"：白名单之外的重复、或白名单项的计数变大，都算失败。
$knownDuplicates = @{ 'A' = 2; 'E' = 2; 'P' = 2; 'S' = 2 }
$duplicates = @($keys | Group-Object | Where-Object { $_.Count -gt 1 })
$unexpected = @($duplicates | Where-Object {
        -not $knownDuplicates.ContainsKey($_.Name) -or $_.Count -gt $knownDuplicates[$_.Name] })
Report ($keys.Count -ge 10 -and $unexpected.Count -eq 0) `
    "右键菜单加速键无新增重复（文案取自宽表，共 $($keys.Count) 个，历史重复 $($duplicates.Count) 个）"
$unexpected | ForEach-Object { Write-Host "    新增重复: $($_.Name) x $($_.Count)" }

# 9. 版本号一致
$mainCpp = Read-Source "JarkViewer/src/main.cpp"
$rc = Get-Content -Raw -LiteralPath (Join-Path $Root "JarkViewer/JarkViewer.rc") -Encoding Unicode
$appVersion = [regex]::Match($mainCpp, 'appVersion = L"v([0-9.]+)"').Groups[1].Value
$stringVersion = [regex]::Match($rc, 'VALUE "FileVersion", "([0-9.]+)"').Groups[1].Value
$numberVersion = [regex]::Match($rc, 'FILEVERSION ([0-9,]+)').Groups[1].Value
$stringMajorMinor = ($stringVersion -split '\.')[0..1] -join '.'
$numberMajorMinor = (($numberVersion -split ',')[0..1] -join '.')
Report ($appVersion -ne "" -and $stringMajorMinor -eq $appVersion -and $numberMajorMinor -eq $appVersion) `
    "版本号一致（main v$appVersion / rc 串 $stringVersion / rc 数 $numberVersion）"

Write-Host ""
if ($script:failed) {
    Write-Host "源码不变量检查存在失败项" -ForegroundColor Red
    exit 1
}
Write-Host "源码不变量检查全部通过" -ForegroundColor Green
exit 0
