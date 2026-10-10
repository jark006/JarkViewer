# 源码不变量检查：查一批"跑起来也看不出来、但一改就会悄悄坏"的约定。
# 只读源码，几秒钟出结果；提交前可单独跑。反向验证见 verify_source_invariant_checks.ps1
# （逐项制造错误确认真会报错——静态检查最危险的失效方式是写歪了正则照样 PASS）。
#
# 检查项：
#   1/2. 两张字符串表的 // N 编号与实际下标一致（编号被硬编码引用，中间插条目会整体错位）
#   3.   默认关联列表（defaultExtList）都在 supportExt/supportRaw 里
#   4/5. 中文 README 的静态/RAW 格式清单与代码里的集合完全一致
#   6/6b. 视频/音频扩展名清单（代码 ↔ README）一致，且音频不得混进看图/视频清单
#   6c.  英文 README 的静态/RAW/视频/音频四张清单与代码一致（它曾落后中文一整轮）
#   7.   PSD 解码顺序是 psd_sdk 优先、stb 兜底（反了 16 位 RLE 会全透明且不报错）
#   8.   ImageDatabase 析构里先停 LRU 预读线程（不然退出时在途解码访问已释放成员）
#   9.   右键菜单加速键不重复（重复时只有先出现的那个能按）
#   10.  main.cpp / .rc 字符串版 / .rc 数字版 版本号一致（升级时最容易漏改 .rc 数字）
#   11.  VS 工程收齐了自有源码/头文件，且 .filters 与 .vcxproj 条目一一对应、路径都存在
#        （漏收录的文件在 VS 里根本看不到，重命名的旧条目会指向不存在的文件）
#   12.  所有 .ps1 带 UTF-8 BOM 且含 PowerShell 7 守卫（无 BOM 时 5.1 会在跑到守卫前就语法报错）
#   13.  两个工程的每个配置都开了多处理器编译 /MP（丢了它全量重编退回单核，且不会有任何报错）
#
# 只支持 PowerShell 7（pwsh）：下面的守卫会拒绝 Windows PowerShell 5.1，并提示怎么装 pwsh。
# 本文件必须保留 UTF-8 BOM——5.1 会把无 BOM 的 .ps1 按 ANSI 解码，脚本在跑到守卫之前就已经
# 乱码/语法报错，用户看到的是一句莫名其妙的报错，而不是这条提示。

param([string]$Root = (Split-Path -Parent $PSScriptRoot))

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
$videoExt = Get-ExtSet $imageDbHeader "videoExt{"
$defaultList = [regex]::Match($jarkUtils, 'defaultExtList\{\s*"([^"]+)"').Groups[1].Value -split ','
$missingDefaults = @($defaultList | Where-Object { -not $supportExt.Contains($_) -and -not $supportRaw.Contains($_) })
Report ($missingDefaults.Count -eq 0) "默认关联列表全部在 supportExt/supportRaw 里"
$missingDefaults | ForEach-Object { Write-Host "    缺失: $_" }

# 4/5. 中文 README 格式清单
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

# 6. 视频扩展名三处一致：ImageDatabase::videoExt、FormatSniffer 判为 Video 的扩展名表、README 视频清单。
#    这一条是"视频归独立播放器、图片走看图"的唯一判定（jark::isVideoFile 查 FormatSniffer 那张表），
#    三处一旦走偏就会出现"列表里点开却跳到播放器"或"进了播放器又说不是视频"。
$readmeVideo = Get-ReadmeList '- \*\*视频\*\*[^`]*`([^`]+)`'
$snifferCpp = Read-Source "JarkViewer/src/FormatSniffer.cpp"
$snifferVideo = [System.Collections.Generic.HashSet[string]]::new()
foreach ($match in [regex]::Matches($snifferCpp, 'L"([a-z0-9]+)",\s*FileFormat::Video')) {
    [void]$snifferVideo.Add($match.Groups[1].Value)
}
$videoDiff = @()
foreach ($ext in $videoExt) {
    if (-not $snifferVideo.Contains($ext)) { $videoDiff += "FormatSniffer 没把 $ext 判为 Video" }
    if (-not $readmeVideo.Contains($ext)) { $videoDiff += "README 缺 $ext" }
    if ($supportExt.Contains($ext)) { $videoDiff += "$ext 同时在 supportExt 里（看图列表会出现它）" }
}
foreach ($ext in $snifferVideo) { if (-not $videoExt.Contains($ext)) { $videoDiff += "videoExt 缺 $ext" } }
foreach ($ext in $readmeVideo) { if (-not $videoExt.Contains($ext)) { $videoDiff += "README 多 $ext" } }
Report ($videoDiff.Count -eq 0) "视频扩展名一致：videoExt == FormatSniffer[Video] == README（$($videoExt.Count) 项）"
$videoDiff | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }

# 6b. 音频扩展名两处一致：FormatSniffer 判为 Audio 的扩展名表、README 音频清单。
#     音频没有 ImageDatabase 侧的集合（播放器只按扩展名判定 jark::isAudioFile），
#     所以是两处而不是三处；两份名单走偏的表现是"README 说支持、实际打不开"
#     或"能打开却不在宣传清单里"。音频扩展名**不能**进 supportExt/videoExt：
#     进了 supportExt 就会出现在看图翻页列表里（点开却跳到播放器，规则就不唯一了）。
$readmeAudio = Get-ReadmeList '- \*\*音频\*\*[^`]*`([^`]+)`'
$snifferAudio = [System.Collections.Generic.HashSet[string]]::new()
foreach ($match in [regex]::Matches($snifferCpp, 'L"([a-z0-9]+)",\s*FileFormat::Audio')) {
    [void]$snifferAudio.Add($match.Groups[1].Value)
}
$audioDiff = @()
foreach ($ext in $snifferAudio) {
    if (-not $readmeAudio.Contains($ext)) { $audioDiff += "README 缺 $ext" }
    if ($supportExt.Contains($ext)) { $audioDiff += "$ext 同时在 supportExt 里（看图列表会出现它）" }
    if ($videoExt.Contains($ext)) { $audioDiff += "$ext 同时在 videoExt 里（该按音频还是视频判？）" }
}
foreach ($ext in $readmeAudio) { if (-not $snifferAudio.Contains($ext)) { $audioDiff += "FormatSniffer 没把 $ext 判为 Audio" } }
Report ($audioDiff.Count -eq 0) "音频扩展名一致：FormatSniffer[Audio] == README（$($snifferAudio.Count) 项）"
$audioDiff | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }

# 6c. 英文 README 的四张格式清单：静态/RAW 对 supportExt/supportRaw，视频/音频对
#     FormatSniffer 的表（视频还与 videoExt 相同）。英文那份**曾经落后中文一整轮**——
#     加了独立播放器与音频支持却没同步，所以单独查一遍，两处清单必须逐项相同。
function Compare-ExtList([string]$text, [string]$pattern, $expected, [string]$label) {
    $diffs = @()
    $found = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($item in ([regex]::Match($text, $pattern).Groups[1].Value -split ' ')) {
        if ($item) { [void]$found.Add($item) }
    }
    if ($found.Count -eq 0) { return @("$label 清单读不出来（正则没匹配上）") }
    foreach ($ext in $expected) { if (-not $found.Contains($ext)) { $diffs += "$label 缺 $ext" } }
    foreach ($ext in $found) { if (-not $expected.Contains($ext)) { $diffs += "$label 多 $ext" } }
    return $diffs
}

$readmeEn = Read-Source "README_EN.md"
$enDiff = @()
$enDiff += Compare-ExtList $readmeEn '- +\*\*Static\*\*[^`]*`([^`]+)`' $supportExt "静态"
$enDiff += Compare-ExtList $readmeEn '- +\*\*RAW\*\*[^`]*`([^`]+)`' $supportRaw "RAW"
$enDiff += Compare-ExtList $readmeEn '- +\*\*Video\*\*[^`]*`([^`]+)`' $videoExt "视频"
$enDiff += Compare-ExtList $readmeEn '- +\*\*Audio\*\*[^`]*`([^`]+)`' $snifferAudio "音频"
Report ($enDiff.Count -eq 0) `
    "README_EN 四张格式清单与代码一致（静态 $($supportExt.Count) / RAW $($supportRaw.Count) / 视频 $($videoExt.Count) / 音频 $($snifferAudio.Count) 项）"
$enDiff | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }

# 7. PSD 解码顺序
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

# 8. LRU 预读线程在派生类析构里先停
$dtorIndex = $imageDbHeader.IndexOf("~ImageDatabase()")
if ($dtorIndex -ge 0) {
    $dtorBlock = $imageDbHeader.Substring($dtorIndex, [Math]::Min(400, $imageDbHeader.Length - $dtorIndex))
    Report ($dtorBlock.Contains("stopPreloadWorker")) "ImageDatabase 析构先停预读线程"
}
else {
    Report $false "找不到 ~ImageDatabase"
}

# 9. 右键菜单加速键唯一（文案在宽表里：先取 CreateContextMenu 引用的 ID，再看这些文案的 (&X)）
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

# 10. 版本号一致
$mainCpp = Read-Source "JarkViewer/src/main.cpp"
$rc = Get-Content -Raw -LiteralPath (Join-Path $Root "JarkViewer/JarkViewer.rc") -Encoding Unicode
$appVersion = [regex]::Match($mainCpp, 'appVersion = L"v([0-9.]+)"').Groups[1].Value
$stringVersion = [regex]::Match($rc, 'VALUE "FileVersion", "([0-9.]+)"').Groups[1].Value
$numberVersion = [regex]::Match($rc, 'FILEVERSION ([0-9,]+)').Groups[1].Value
$stringMajorMinor = ($stringVersion -split '\.')[0..1] -join '.'
$numberMajorMinor = (($numberVersion -split ',')[0..1] -join '.')
Report ($appVersion -ne "" -and $stringMajorMinor -eq $appVersion -and $numberMajorMinor -eq $appVersion) `
    "版本号一致（main v$appVersion / rc 串 $stringVersion / rc 数 $numberVersion）"

# 11. VS 工程文件列表（漏收录的文件在 VS 里看不到、IntelliSense 也跳不过去；改文件名后
#     .filters 里的旧条目会指向不存在的路径）。两个工程都查。
$itemPattern = '<(?:ClCompile|ClInclude|ResourceCompile|Image|None) Include="([^"]+)"'
foreach ($project in @(@{Name = "JarkViewer"; Dir = "JarkViewer" },
                       @{Name = "JarkThumbnailProvider"; Dir = "JarkThumbnailProvider" })) {
    $base = "$($project.Dir)/$($project.Name)"
    $projectItems = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($m in [regex]::Matches((Read-Source "$base.vcxproj"), $itemPattern)) {
        [void]$projectItems.Add($m.Groups[1].Value)
    }
    $filterItems = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($m in [regex]::Matches((Read-Source "$base.vcxproj.filters"), $itemPattern)) {
        [void]$filterItems.Add($m.Groups[1].Value)
    }

    $ownMissing = @()
    foreach ($dir in @("src", "include")) {
        $fullDir = Join-Path $Root "$($project.Dir)/$dir"
        if (-not (Test-Path -LiteralPath $fullDir)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $fullDir -File) {
            if ($file.Extension -notin @(".cpp", ".h")) { continue }
            # include 根目录下的第三方单文件头也要收录（归到"第三方"）；库目录里的头不查
            $relative = "$dir\$($file.Name)"
            if (-not $projectItems.Contains($relative)) { $ownMissing += $relative }
        }
    }

    $staleInProject = @($projectItems | Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $Root "$($project.Dir)/$_")) })
    $filtersMismatch = @($projectItems | Where-Object { -not $filterItems.Contains($_) })

    Report ($ownMissing.Count -eq 0) "$($project.Name)：工程收录了所有自有源码/头文件（漏收 $($ownMissing.Count)）"
    Report ($staleInProject.Count -eq 0) "$($project.Name)：工程条目都指向存在的文件（失效 $($staleInProject.Count)）"
    Report ($filtersMismatch.Count -eq 0) "$($project.Name)：.vcxproj 与 .filters 条目一一对应（缺 $($filtersMismatch.Count)）"
    $ownMissing | Select-Object -First 5 | ForEach-Object { Write-Host "    漏收录: $_" }
    $staleInProject | Select-Object -First 5 | ForEach-Object { Write-Host "    失效条目: $_" }
    $filtersMismatch | Select-Object -First 3 | ForEach-Object { Write-Host "    缺筛选器条目: $_" }
}

# 12. 所有 .ps1 都带 UTF-8 BOM，且有 PowerShell 7 守卫。
#     BOM 不是洁癖：5.1 把无 BOM 的 .ps1 按 ANSI 解码，中文注释会先把它自己解析崩，
#     用户看到的是"表达式或语句中的 '.' 后缺少表达式"这种莫名其妙的报错，而不是守卫
#     那句"请改用 pwsh"。守卫被删/被改则等于放 5.1 进来跑（buildRelease 会静默退化成
#     Debug 构建）。这两个都是"改了看不出来、只有在 5.1 上跑才炸"的约定。
$psScripts = @(Get-ChildItem -LiteralPath $Root -Filter *.ps1 -File) +
    @(Get-ChildItem -LiteralPath (Join-Path $Root "tools") -Filter *.ps1 -File)
$psGuard = '$PSVersionTable.PSEdition -ne ' + "'Core'"
$psProblems = @()
foreach ($script in $psScripts) {
    $bytes = [IO.File]::ReadAllBytes($script.FullName)
    if ($bytes.Length -lt 3 -or $bytes[0] -ne 0xEF -or $bytes[1] -ne 0xBB -or $bytes[2] -ne 0xBF) {
        $psProblems += "$($script.Name) 缺 UTF-8 BOM"
    }
    if (-not (Get-Content -Raw -LiteralPath $script.FullName -Encoding UTF8).Contains($psGuard)) {
        $psProblems += "$($script.Name) 缺 PowerShell 7 守卫"
    }
}
Report ($psProblems.Count -eq 0) "所有 .ps1 带 UTF-8 BOM 且有 PowerShell 7 守卫（$($psScripts.Count) 个）"
$psProblems | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }

# 13. 两个工程都要开多处理器编译（/MP = `<MultiProcessorCompilation>true</...>`）。
#     MSBuild 的 /m 只在**工程之间**并行，本解决方案就两个工程、看图独占大头；工程内部的
#     .cpp 是靠 /MP 才并行的。这个开关掉了不会有任何报错，只是全量重编退回单核（实测 8 个
#     源文件：关着 max cl=1 / 36s，开着 max cl=9 / 14s），所以专门盯一下。
$mpMissing = @()
foreach ($project in @("JarkViewer/JarkViewer.vcxproj", "JarkThumbnailProvider/JarkThumbnailProvider.vcxproj")) {
    $text = Read-Source $project
    $configs = ([regex]::Matches($text, '<ItemDefinitionGroup')).Count
    $enabled = ([regex]::Matches($text, '<MultiProcessorCompilation>true</MultiProcessorCompilation>')).Count
    if ($enabled -lt $configs) { $mpMissing += "$($project) 只有 $enabled/$configs 个配置开了 /MP" }
}
Report ($mpMissing.Count -eq 0) "两个工程的每个配置都开了多处理器编译 /MP"
$mpMissing | ForEach-Object { Write-Host "    $_" }

Write-Host ""
if ($script:failed) {
    Write-Host "源码不变量检查存在失败项" -ForegroundColor Red
    exit 1
}
Write-Host "源码不变量检查全部通过" -ForegroundColor Green
exit 0
