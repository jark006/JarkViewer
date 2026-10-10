# Catch an intermittent crash of JarkViewer.exe by attaching cdb AFTER the process
# has started, and write a full memory dump when it faults.
#
# Why "attach after": launching the program *under* a debugger hides timing-sensitive
# bugs (measured: 4/10 crashes when started normally, 0/10 when started under cdb).
#
# Usage:
#   pwsh tools/catch_crash.ps1 -Exe x64/Release/JarkViewer.exe `
#        -AppArgs="--probe --video-test A.mp4 B.mp4 --out report.txt" `
#        -Dump "$env:TEMP\crash.dmp" -Attempts 8
#
# Note the `-AppArgs="..."` form: PowerShell would otherwise read a value that
# starts with a dash (--probe) as another parameter name and fail to bind it.
#
# Then analyze offline:
#   cdb.exe -z <dump> -c ".ecxr;r;kb 12;q"      (.ecxr is required: the default
#                                                thread is not the faulting one)
#
# PowerShell 7 (pwsh) only -- the guard below refuses Windows PowerShell 5.1 and says how to
# install pwsh. Keep the UTF-8 BOM on this file: 5.1 decodes BOM-less .ps1 as ANSI, which mangles
# the script before the guard can run (it dies with a syntax error instead of the hint).
# NOTE: a faulting program only produces a dump if it actually crashes; when the
#       debugger masks the bug this script just runs to the end without a dump.
# NOTE: attaching masks the timing-sensitive crashes too (measured 1/5 normally vs
#       almost never once attached). For those, let the process dump itself:
#         JARKVIEWER_CRASH_DUMP=<file> JarkViewer.exe --probe ...
#       (see the "JARKVIEWER_CRASH_DUMP" bullet in AGENTS.md).

param(
    [string]$Exe = "x64/Release/JarkViewer.exe",
    [string]$AppArgs = "",
    [string]$Dump = "$env:TEMP\jv-crash.dmp",
    [int]$Attempts = 6,
    [int]$AttachDelayMs = 900,
    [string]$Cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
    [string]$LogDir = "$env:TEMP"
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

if (-not (Test-Path $Cdb)) { throw "cdb.exe not found: $Cdb" }

$exePath = (Resolve-Path -LiteralPath $Exe).Path
$argList = @()
if ($AppArgs.Trim().Length -gt 0) { $argList = $AppArgs -split '\s+' }

for ($i = 1; $i -le $Attempts; $i++) {
    Remove-Item -LiteralPath $Dump -ErrorAction SilentlyContinue
    $log = Join-Path $LogDir ("catch-crash-$i.log")

    $proc = Start-Process -FilePath $exePath -ArgumentList $argList -PassThru -NoNewWindow
    Start-Sleep -Milliseconds $AttachDelayMs

    if (-not $proc.HasExited) {
        # "g" resumes; the rest runs when the debugger breaks in again (i.e. on the fault).
        # "q" quits (and kills the debuggee) - the dump is already on disk by then.
        & $Cdb -p $proc.Id -c "g;.dump /ma `"$Dump`";q" -logo $log | Out-Null
    }
    $proc.WaitForExit()

    if (Test-Path -LiteralPath $Dump) {
        Write-Output "attempt ${i}: dump written -> $Dump (cdb log: $log)"
        exit 0
    }
    Write-Output ("attempt {0}: no crash (exit code {1})" -f $i, $proc.ExitCode)
}

Write-Output "no crash in $Attempts attempts (is the debugger masking it? check >$LogDir\catch-crash-*.log)"
exit 1
