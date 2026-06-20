param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) {
    $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe"
}

if (-not (Test-Path $csc)) {
    throw "Could not find the .NET Framework C# compiler. Install .NET Framework Developer Pack or the .NET SDK."
}

$optimize = if ($Configuration -eq "Release") { "/optimize+" } else { "/optimize-" }

New-Item -ItemType Directory -Force -Path .\publish | Out-Null

& $csc `
    /nologo `
    /target:winexe `
    /platform:x64 `
    $optimize `
    /codepage:65001 `
    /utf8output `
    /win32manifest:app.manifest `
    /reference:System.dll `
    /reference:System.Drawing.dll `
    /reference:System.Windows.Forms.dll `
    /out:.\publish\DimScreensaver.exe `
    .\Program.cs

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

Copy-Item .\publish\DimScreensaver.exe .\publish\DimScreensaver.scr -Force
Copy-Item .\DimScreensaver.ini .\publish\DimScreensaver.ini -Force

Write-Host "Built .\publish\DimScreensaver.scr"
