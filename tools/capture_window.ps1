# Capture a single window to a PNG without capturing the rest of the desktop.
#
# Usage:
#   pwsh tools/capture_window.ps1 -Exe x64/Release/JarkViewer.exe -Argument "image.svg" -Out shot.png
#
# The target process is started with -Argument, its main window is captured with
# PrintWindow(PW_RENDERFULLCONTENT) and the process is killed afterwards.
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI).

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Argument = "",
    [string]$Keys = "",
    [int]$WaitMs = 2500,
    [int]$AfterKeysMs = 1200,
    [int]$TimeoutMs = 15000
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class JarkCapture {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
"@

# The host must be DPI-aware, otherwise GetWindowRect reports virtualized (logical)
# coordinates and the capture ends up as a downscaled top-left crop of a high-DPI window.
[void][JarkCapture]::SetProcessDPIAware()

$proc = Start-Process -FilePath $Exe -ArgumentList $Argument -PassThru
try {
    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $proc.Refresh()
        if ($proc.HasExited) { throw "process exited early with code $($proc.ExitCode)" }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle; break }
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "main window not found within $TimeoutMs ms" }

    Start-Sleep -Milliseconds $WaitMs

    if ($Keys -ne "") {
        [void][JarkCapture]::SetForegroundWindow($hwnd)
        Start-Sleep -Milliseconds 300
        if ([JarkCapture]::GetForegroundWindow() -eq $hwnd) {
            [System.Windows.Forms.SendKeys]::SendWait($Keys)
            Start-Sleep -Milliseconds $AfterKeysMs
        }
        else {
            Write-Warning "window did not come to the foreground; keys skipped"
        }
    }

    $rect = New-Object JarkCapture+RECT
    if (-not [JarkCapture]::GetWindowRect($hwnd, [ref]$rect)) { throw "GetWindowRect failed" }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { throw "invalid window size ${width}x${height}" }

    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    $printed = [JarkCapture]::PrintWindow($hwnd, $hdc, 2)   # PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()

    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()

    "captured=$printed size=${width}x${height} out=$Out"
}
finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
