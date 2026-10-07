# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads BOM-less .ps1 files as
# ANSI, and a mangled multi-byte character can swallow the following newline.
#
# NOTE: Windows PowerShell 5.1 does not support ProcessStartInfo.ArgumentList.
# Using ArgumentList.Add() there silently drops every argument, which makes MSBuild
# fall back to a no-argument build (i.e. the default Debug configuration).
# Pass arguments through the Arguments string instead.

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
