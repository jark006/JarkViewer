# Verify the command-line language override (`--lang N`) and that it stays TEMPORARY:
#   1) with no arguments the home page uses the default language (Simplified Chinese);
#   2) `--lang 3` renders the home page in Japanese -- the override must survive the settings
#      file, which the window reads AFTER the command line has been parsed (regression: the
#      file used to overwrite the override, so `--lang` did nothing in the GUI);
#   3) after quitting a `--lang 3` session (settings are written on exit), the next launch
#      without arguments is back to the default language -- the temporary language must not
#      have been persisted to JarkViewer.db.
#
# The home page is compared by the pixels of its button area: different languages draw a
# different number of glyphs there, so no OCR is needed.
#
# Why a temp copy of the exe: the app writes JarkViewer.db next to the executable, so running
# this against the repo build would touch the user's own settings. The copy starts with no
# settings file, which makes the baseline deterministic (default language).
#
# Usage: pwsh tools/test_language_override.ps1 -Exe x64/Release/JarkViewer.exe
# PowerShell 7 (pwsh) only -- the guard below refuses Windows PowerShell 5.1 and says how to
# install pwsh. Keep the UTF-8 BOM on this file: 5.1 decodes BOM-less .ps1 as ANSI, which mangles
# the script before the guard can run (it dies with a syntax error instead of the hint).


param(
    [string]$Exe = "x64/Release/JarkViewer.exe",
    [string]$WorkDir = "$env:TEMP\jv-language-test"
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

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$script:failed = $false

function Report([bool]$ok, [string]$name) {
    if ($ok) { Write-Host "  [ok] $name" }
    else { Write-Host "  [FAIL] $name" -ForegroundColor Red; $script:failed = $true }
}

$exeFull = (Resolve-Path $Exe).Path
$captureScript = Join-Path $PSScriptRoot "capture_window.ps1"

if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Path $WorkDir | Out-Null
$workExe = Join-Path $WorkDir "JarkViewer.exe"
Copy-Item $exeFull $workExe

function Invoke-Capture([string]$name, [string]$argument) {
    $out = Join-Path $WorkDir $name
    $captureArgs = @("-NoProfile", "-File", $captureScript, "-Exe", $workExe, "-Out", $out)
    if ($argument) { $captureArgs += @("-Argument", $argument) }
    & pwsh @captureArgs | Out-Null
    if (-not (Test-Path $out)) { throw "capture failed: $name" }
    return $out
}

# Share of differing pixels inside the home-page button area (where the language shows up).
function Get-ButtonDiff([string]$pathA, [string]$pathB) {
    $a = [System.Drawing.Bitmap]::FromFile($pathA)
    $b = [System.Drawing.Bitmap]::FromFile($pathB)
    try {
        $diff = 0
        $total = 0
        $y0 = [int]($a.Height * 0.58); $y1 = [int]($a.Height * 0.66)
        $x0 = [int]($a.Width * 0.43); $x1 = [int]($a.Width * 0.57)
        for ($y = $y0; $y -lt $y1; $y += 2) {
            for ($x = $x0; $x -lt $x1; $x += 2) {
                $ca = $a.GetPixel($x, $y)
                $cb = $b.GetPixel($x, $y)
                $delta = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
                if ($delta -gt 40) { $diff++ }
                $total++
            }
        }
        if ($total -eq 0) { return 0.0 }
        return $diff / [double]$total
    }
    finally {
        $a.Dispose()
        $b.Dispose()
    }
}

$baseShot = Invoke-Capture "home_default.png" ""
$jaShot = Invoke-Capture "home_japanese.png" "--lang 3"

# Quit a --lang 3 session through WM_CLOSE so saveSettings() actually runs.
$proc = Start-Process -FilePath $workExe -ArgumentList "--lang 3" -PassThru
Start-Sleep -Seconds 4
$null = $proc.CloseMainWindow()
if (-not $proc.WaitForExit(15000)) { $proc.Kill() }
$afterShot = Invoke-Capture "home_after.png" ""

$changed = Get-ButtonDiff $baseShot $jaShot
$persisted = Get-ButtonDiff $baseShot $afterShot
Write-Host ("button-area pixel diff: --lang 3 -> {0:P1}, after restart -> {1:P1}" -f $changed, $persisted)

Report ($changed -gt 0.02) "`--lang 3 actually changes the UI language"
Report ($persisted -lt 0.01) "the temporary language is NOT persisted (restart is back to default)"

Write-Host ""
if ($script:failed) {
    Write-Host "language override regression FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "language override regression passed" -ForegroundColor Green
exit 0
