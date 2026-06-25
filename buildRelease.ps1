$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
$solution = Join-Path $PSScriptRoot "JarkViewer.slnx"

$psi = [System.Diagnostics.ProcessStartInfo]::new($msbuild)
@($solution, "/m", "/p:Configuration=Release", "/p:Platform=x64", "/v:m") | ForEach-Object {
    [void]$psi.ArgumentList.Add($_)
}

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
