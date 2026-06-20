param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $projectDir
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $candidateRoots = @()

    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = & $vswhere -latest -products * -property installationPath
        if ($installationPath) {
            $candidateRoots += $installationPath
        }
    }

    $candidateRoots += @(
        "$env:ProgramFiles\Microsoft Visual Studio\18\Community",
        "$env:ProgramFiles\Microsoft Visual Studio\18\Professional",
        "$env:ProgramFiles\Microsoft Visual Studio\18\Enterprise",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise"
    )

    $vcvars = $null
    foreach ($root in $candidateRoots | Where-Object { $_ } | Select-Object -Unique) {
        $candidate = Join-Path $root "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path -LiteralPath $candidate) {
            $vcvars = $candidate
            break
        }
    }

    if (-not $vcvars) {
        throw "Could not find vcvars64.bat. Install the Desktop development with C++ workload in Visual Studio."
    }

    New-Item -ItemType Directory -Force -Path .\publish | Out-Null

    $optimization = if ($Configuration -eq "Release") { "/O2 /MT" } else { "/Od /Zi /MTd" }
    $compile = "cl.exe /nologo /W4 /std:c17 /TC /DUNICODE /D_UNICODE $optimization /Fo:publish\ dim_screensaver.c /link /SUBSYSTEM:WINDOWS /OUT:publish\DimScreensaver.exe user32.lib gdi32.lib shell32.lib ole32.lib windowscodecs.lib uuid.lib"
    $manifest = "mt.exe -nologo -manifest app.manifest -outputresource:publish\DimScreensaver.exe;#1"
    $command = "call `"$vcvars`" >nul && $compile && $manifest"

    cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    Copy-Item .\publish\DimScreensaver.exe .\publish\DimScreensaver.scr -Force
    Copy-Item .\DimScreensaver.ini .\publish\DimScreensaver.ini -Force
    Write-Host "Built .\publish\DimScreensaver.scr"
}
finally {
    Pop-Location
}
