# 反向验证 check_source_invariants.ps1：逐项制造它该抓的错误，确认真会报错，
# 跑完按原字节还原（不用 git checkout——那会连未提交的改动一起抹掉）。
# 静态检查最危险的失效方式是"什么都抓不到"：正则写歪一个字符就照样打印 PASS，
# 所以每加一条检查，就在这个脚本里加一条对应的破坏。
param([string]$Root = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = "Stop"
$script:failed = $false
$checkScript = Join-Path $PSScriptRoot "check_source_invariants.ps1"

function Test-Break([string]$name, [hashtable]$edits) {
    # $edits: 相对路径 -> @(old, new)；多文件各自替换
    $backup = @{}
    $checkerFailed = $false
    try {
        foreach ($path in $edits.Keys) {
            $full = Join-Path $Root $path
            $backup[$path] = [IO.File]::ReadAllBytes($full)

            if ($path -like "*.rc") {
                $text = [IO.File]::ReadAllText($full, [Text.Encoding]::Unicode)
                $encoding = [Text.Encoding]::Unicode
            }
            else {
                $text = [IO.File]::ReadAllText($full, [Text.Encoding]::UTF8)
                $encoding = New-Object Text.UTF8Encoding($true)
            }

            # 行尾自适应：片段统一按文件的实际情况换行（工作区里 CRLF / LF 都存在）
            $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
            $old = $edits[$path][0].Replace("`r`n", "`n").Replace("`n", $nl)
            $new = $edits[$path][1].Replace("`r`n", "`n").Replace("`n", $nl)
            if (-not $text.Contains($old)) { throw "找不到待破坏片段: $path ($($old.Substring(0, [Math]::Min(40, $old.Length))))" }
            [IO.File]::WriteAllText($full, $text.Replace($old, $new), $encoding)
        }

        & pwsh -NoProfile -File $checkScript *> $null
        $checkerFailed = ($LASTEXITCODE -ne 0)
    }
    finally {
        foreach ($path in $backup.Keys) {
            [IO.File]::WriteAllBytes((Join-Path $Root $path), $backup[$path])
        }
    }

    if ($checkerFailed) {
        Write-Host "  [ok] $name"
    }
    else {
        Write-Host "  [FAIL] $name —— 检查没抓到破坏" -ForegroundColor Red
        $script:failed = $true
    }
}

Write-Host "== 反向验证源码不变量检查 =="

Test-Break "窄表 // N 编号改错" @{
    "JarkViewer/src/stringRes.cpp" = @("// 173  EXIF 方向：正常", "// 999  EXIF 方向：正常")
}

Test-Break "宽表 // N 编号改错" @{
    "JarkViewer/src/stringRes.cpp" = @('"설정", "Настройки"}, // 39', '"설정", "Настройки"}, // 40')
}

Test-Break "默认关联列表混入不支持的扩展名" @{
    "JarkViewer/include/jarkUtils.h" = @('webp,wp2" };', 'webp,wp2,fakeext" };')
}

Test-Break "README 静态清单缺一个格式" @{
    "README.md" = @("lep livp", "livp")
}

Test-Break "README RAW 清单多一个格式" @{
    "README.md" = @("srw x3f", "srw x3f x9z")
}

Test-Break "PSD 解码顺序颠倒" @{
    "JarkViewer/src/ImageDatabase.cpp" = @(
        "        auto img = loadPSD(path, buf);`r`n        if (img.empty())`r`n            img = loadSTB(path, buf);",
        "        auto img = loadSTB(path, buf);`r`n        if (img.empty())`r`n            img = loadPSD(path, buf);")
}

Test-Break "ImageDatabase 析构删除 stopPreloadWorker" @{
    "JarkViewer/include/ImageDatabase.h" = @("        stopPreloadWorker();`r`n", "")
}

Test-Break "菜单加速键新增重复" @{
    "JarkViewer/src/stringRes.cpp" = @("移动到目标文件夹 (&M)", "移动到目标文件夹 (&T)")
}

Test-Break "版本号数字版与字符串版不一致" @{
    "JarkViewer/JarkViewer.rc" = @("FILEVERSION 2,0,0,0", "FILEVERSION 1,35,0,0")
}

Write-Host ""
# 全部还原后再跑一遍完整检查，确认真文件恢复了
& pwsh -NoProfile -File $checkScript *> $null
$restored = ($LASTEXITCODE -eq 0)

if ($script:failed -or -not $restored) {
    Write-Host "反向验证存在失败项（或还原后检查未通过）" -ForegroundColor Red
    exit 1
}
Write-Host "反向验证全部通过：9 项破坏全被抓到，文件已按原字节还原" -ForegroundColor Green
exit 0
