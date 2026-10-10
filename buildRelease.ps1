# PowerShell 7 (pwsh) only -- the guard below refuses Windows PowerShell 5.1 and says how to
# install pwsh. Keep the UTF-8 BOM on this file: 5.1 decodes BOM-less .ps1 as ANSI, which mangles
# the script before the guard can run (it dies with a syntax error instead of the hint).
# Build arguments go through the ProcessStartInfo.Arguments string (not ArgumentList):
# it behaves the same on every supported host and keeps the command line in one place.

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
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
$solution = Join-Path $PSScriptRoot "JarkViewer.slnx"

$psi = [System.Diagnostics.ProcessStartInfo]::new($msbuild)
$psi.Arguments = '"' + $solution + '" /m /p:Configuration=Release /p:Platform=x64 /v:m'

$psi.WorkingDirectory = $PSScriptRoot
$psi.UseShellExecute = $false

$envItems = Get-ChildItem Env: | Sort-Object Name -Unique
$psi.Environment.Clear()
foreach ($item in $envItems) {
    if (-not $psi.Environment.ContainsKey($item.Name)) {
        $psi.Environment[$item.Name] = $item.Value
    }
}

if ($psi.Environment.ContainsKey("PATH")) {
    $pathValue = $psi.Environment["PATH"]
    [void]$psi.Environment.Remove("PATH")
    $psi.Environment["Path"] = $pathValue
}

$process = [System.Diagnostics.Process]::Start($psi)
$process.WaitForExit()
exit $process.ExitCode
