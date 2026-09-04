<#
.SYNOPSIS
    Compiles DLSS5Live.dll for Foundry Nuke 15 & 17 using CMake and MSVC.
#>
param(
    [string]$Nuke15Dir = "C:\Program Files\Nuke15.0v2",
    [string]$Nuke17Dir = "C:\Program Files\Nuke17.0v1",
    [string]$ToolsDir  = $env:DLSS5_TOOLS_DIR
)

$ErrorActionPreference = "Stop"
$projectRoot = Resolve-Path "$PSScriptRoot\.."
$binDir = Join-Path $projectRoot "bin"
$bin15 = Join-Path $binDir "Nuke15"
$bin17 = Join-Path $binDir "Nuke17"

if (-not (Test-Path $bin15)) { New-Item -ItemType Directory -Force -Path $bin15 | Out-Null }
if (-not (Test-Path $bin17)) { New-Item -ItemType Directory -Force -Path $bin17 | Out-Null }

# Locate build tools (vcvars64, cmake, ninja)
$vcvars = $null
$cmake = "cmake.exe"
$ninja = "ninja.exe"

if (-not $ToolsDir) {
    $pkgTools = Join-Path $projectRoot "package\tools"
    if (Test-Path $pkgTools) { $ToolsDir = $pkgTools }
}

if ($ToolsDir -and (Test-Path $ToolsDir)) {
    if (Test-Path (Join-Path $ToolsDir "msvc\vcvars64.bat")) { $vcvars = Join-Path $ToolsDir "msvc\vcvars64.bat" }
    if (Test-Path (Join-Path $ToolsDir "cmake\bin\cmake.exe")) { $cmake = Join-Path $ToolsDir "cmake\bin\cmake.exe" }
    if (Test-Path (Join-Path $ToolsDir "ninja\ninja.exe")) { $ninja = Join-Path $ToolsDir "ninja\ninja.exe" }
}

# Fallback auto-detection for Visual Studio vcvars64
if (-not $vcvars) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $vsInstall = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsInstall) {
            $candidate = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { $vcvars = $candidate }
        }
    }
}

if (-not $vcvars) {
    Write-Warning "Could not find vcvars64.bat automatically. Please set DLSS5_TOOLS_DIR or run from x64 Native Tools Command Prompt."
}

function Safe-CopyFile($src, $dst) {
    try {
        Copy-Item $src $dst -Force
    } catch {
        $oldFile = "$dst.old"
        if (Test-Path $oldFile) {
            try { Remove-Item $oldFile -Force -ErrorAction SilentlyContinue } catch {}
        }
        Move-Item $dst $oldFile -Force
        Copy-Item $src $dst -Force
        Write-Host "    [!] $dst locked; rotated to .old and copied new binary." -ForegroundColor Yellow
    }
}

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  Building DLSS 5 LiveOp for Nuke 15 & 17 " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

# 1. Build Nuke 15
if (Test-Path $Nuke15Dir) {
    Write-Host "`n[1/2] Compiling for Nuke 15 ($Nuke15Dir)..." -ForegroundColor Yellow
    $build15 = Join-Path $projectRoot "build_nuke15"
    if (-not (Test-Path $build15)) { New-Item -ItemType Directory -Force -Path $build15 | Out-Null }

    $callVc = if ($vcvars) { "call `"$vcvars`" && " } else { "" }
    $cmd15 = '{0}cd /d "{1}" && "{2}" "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE=Release -DNUKE_INSTALL_DIR="{5}" && "{2}" --build .' -f $callVc, $build15, $cmake, $projectRoot, $ninja, ($Nuke15Dir -replace '\\','/')
    cmd.exe /c $cmd15
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[ERROR] Build failed for Nuke 15!"
        exit $LASTEXITCODE
    }
    Safe-CopyFile (Join-Path $build15 "DLSS5Live.dll") (Join-Path $bin15 "DLSS5Live.dll")
    Write-Host "[SUCCESS] Nuke 15 DLL: $bin15\DLSS5Live.dll" -ForegroundColor Green
} else {
    Write-Host "`n[1/2] Skipping Nuke 15 (Directory not found: $Nuke15Dir)" -ForegroundColor Gray
}

# 2. Build Nuke 17
if (Test-Path $Nuke17Dir) {
    Write-Host "`n[2/2] Compiling for Nuke 17 ($Nuke17Dir)..." -ForegroundColor Yellow
    $build17 = Join-Path $projectRoot "build_nuke17"
    if (-not (Test-Path $build17)) { New-Item -ItemType Directory -Force -Path $build17 | Out-Null }

    $callVc = if ($vcvars) { "call `"$vcvars`" && " } else { "" }
    $cmd17 = '{0}cd /d "{1}" && "{2}" "{3}" -G Ninja -DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE=Release -DNUKE_INSTALL_DIR="{5}" && "{2}" --build .' -f $callVc, $build17, $cmake, $projectRoot, $ninja, ($Nuke17Dir -replace '\\','/')
    cmd.exe /c $cmd17
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[ERROR] Build failed for Nuke 17!"
        exit $LASTEXITCODE
    }
    Safe-CopyFile (Join-Path $build17 "DLSS5Live.dll") (Join-Path $bin17 "DLSS5Live.dll")
    Write-Host "[SUCCESS] Nuke 17 DLL: $bin17\DLSS5Live.dll" -ForegroundColor Green
} else {
    Write-Host "`n[2/2] Skipping Nuke 17 (Directory not found: $Nuke17Dir)" -ForegroundColor Gray
}

Write-Host "`n==========================================" -ForegroundColor Green
Write-Host "  Build completed!" -ForegroundColor Green
Write-Host "==========================================" -ForegroundColor Green
