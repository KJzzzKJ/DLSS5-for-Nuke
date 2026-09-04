<#
.SYNOPSIS
    Packages DLSS5-for-Nuke release folder and generates GitHub Release ZIP.
#>
param(
    [string]$OutputDir = "$PSScriptRoot\..\dist\DLSS5_Nuke",
    [switch]$CreateZip = $false
)

$ErrorActionPreference = "Stop"
$projectRoot = Resolve-Path "$PSScriptRoot\.."

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  DLSS 5 for Nuke - Release Packaging Tool        " -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan

# 1. Prepare clean output directory
if (Test-Path $OutputDir) {
    Remove-Item $OutputDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutputDir\bin\Nuke15" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutputDir\bin\Nuke17" | Out-Null
New-Item -ItemType Directory -Force -Path "$OutputDir\runtime" | Out-Null

# 2. Copy Plugin DLLs
$binDir = Join-Path $projectRoot "bin"
$dllFound = $false

foreach ($nukeVer in @("Nuke15", "Nuke17")) {
    $buildDir = if ($nukeVer -eq "Nuke15") { "build_nuke15" } else { "build_nuke17" }
    $builtDll = Join-Path $projectRoot "$buildDir\DLSS5Live.dll"
    $srcDll = if (Test-Path $builtDll) { $builtDll } else { "$binDir\$nukeVer\DLSS5Live.dll" }
    if (Test-Path $srcDll) {
        Copy-Item $srcDll "$OutputDir\bin\$nukeVer\DLSS5Live.dll" -Force
        if (-not $dllFound) {
            Copy-Item $srcDll "$OutputDir\DLSS5Live.dll" -Force
        }
        Write-Host "[+] Copied $nukeVer DLL (DLSS5Live.dll)" -ForegroundColor Green
        $dllFound = $true
    }
}

# 3. Copy Install Scripts & Documentation
Copy-Item "$projectRoot\install\install.bat" "$OutputDir\install.bat" -Force
Copy-Item "$projectRoot\install\menu.py" "$OutputDir\menu.py" -Force
if (Test-Path "$projectRoot\install\init.py") {
    Copy-Item "$projectRoot\install\init.py" "$OutputDir\init.py" -Force
}
if (Test-Path "$projectRoot\install\DLSS5.png") {
    Copy-Item "$projectRoot\install\DLSS5.png" "$OutputDir\DLSS5.png" -Force
}
if (Test-Path "$projectRoot\README.md") {
    Copy-Item "$projectRoot\README.md" "$OutputDir\README.md" -Force
}
Write-Host "[+] Copied install.bat, menu.py, init.py, DLSS5.png, README.md" -ForegroundColor Green

# 4. Copy Runtime Dependencies
$runtimeCandidates = @(
    "$env:DLSS5_RUNTIME_DIR",
    "$projectRoot\worker\build",
    "$projectRoot\runtime",
    "$projectRoot\package\runtime",
    "$env:USERPROFILE\.nuke\DLSS5Live\runtime",
    "$PSScriptRoot\..\..\DLSS5\package\runtime"
)

$runtimeFiles = @(
    "DLSS_Nuke_Worker.exe",
    "nvngx.dll",
    "nvngx_dlss.dll",
    "nvngx_dlssnr.dll",
    "dxgi.dll",
    "renodx-dlss5.addon64",
    "ReShade.ini"
)

foreach ($file in $runtimeFiles) {
    $copied = $false
    foreach ($candidate in $runtimeCandidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $sourcePath = Join-Path $candidate $file
        if (Test-Path $sourcePath) {
            Copy-Item $sourcePath "$OutputDir\runtime\$file" -Force
            Write-Host "    [+] Runtime: $file" -ForegroundColor Gray
            $copied = $true
            break
        }
    }
    if (-not $copied) {
        Write-Warning "Could not find runtime file: $file"
    }
}

Write-Host "`n[SUCCESS] Package assembled at: $OutputDir" -ForegroundColor Green

# 5. Optional Zip Creation
if ($CreateZip) {
    # Extract version from CMakeLists.txt
    $cmakePath = Join-Path $projectRoot "CMakeLists.txt"
    $version = "1.0.0"
    if (Test-Path $cmakePath) {
        $cmakeContent = Get-Content $cmakePath -Raw
        if ($cmakeContent -match 'project\s*\(\s*DLSS5Live\s+VERSION\s+([0-9\.]+)') {
            $version = $matches[1]
        }
    }

    $distParent = Split-Path -Parent $OutputDir
    $zipPath = Join-Path $distParent "DLSS5-for-Nuke-v$version.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Write-Host "[*] Creating Release ZIP (v$version): $zipPath ..." -ForegroundColor Yellow
    Compress-Archive -Path "$OutputDir\*" -DestinationPath $zipPath -Force
    Write-Host "[SUCCESS] Release ZIP created!" -ForegroundColor Green
    Write-Host "  ZIP: $zipPath" -ForegroundColor Cyan
}

Write-Host "===================================================" -ForegroundColor Cyan
