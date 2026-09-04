$ErrorActionPreference = "Stop"
$scriptDir = $PSScriptRoot
$repoRoot  = Resolve-Path "$scriptDir\.."

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  DLSS Nuke Worker Builder (Native D3D12 + NGX)"     -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan

# 1. Locate toolchain (Priority: Package tools -> System tools)
$pkgTools   = Join-Path $repoRoot "package\tools"
$localVcvars = Join-Path $pkgTools "msvc\vcvars64.bat"
$localCmake  = Join-Path $pkgTools "cmake\bin\cmake.exe"
$localNinja  = Join-Path $pkgTools "ninja\ninja.exe"

$vcvars   = $null
$cmakeExe = "cmake.exe"
$ninjaExe = $null

if (Test-Path $localVcvars) {
    $vcvars = $localVcvars
    Write-Host "[+] Found portable MSVC: $vcvars" -ForegroundColor Green
} else {
    $fallbackPaths = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($p in $fallbackPaths) {
        if (Test-Path $p) { $vcvars = $p; break }
    }
}

if (-not $vcvars) {
    Write-Error "Could not locate vcvars64.bat (neither portable package nor system Visual Studio found)."
    exit 1
}

if (Test-Path $localCmake) {
    $cmakeExe = $localCmake
    Write-Host "[+] Found portable CMake: $cmakeExe" -ForegroundColor Green
}

if (Test-Path $localNinja) {
    $ninjaExe = $localNinja
    Write-Host "[+] Found portable Ninja: $ninjaExe" -ForegroundColor Green
}

$buildDir = Join-Path $scriptDir "build"
if (Test-Path $buildDir) {
    Remove-Item $buildDir -Recurse -Force
}
New-Item -ItemType Directory -Path $buildDir | Out-Null

# 2. Configure CMake
Write-Host "`n[1/2] Configuring CMake..." -ForegroundColor Yellow
if ($ninjaExe) {
    $genArgs = '-G "Ninja" -DCMAKE_MAKE_PROGRAM="{0}" -DCMAKE_BUILD_TYPE=Release' -f ($ninjaExe -replace '\\', '/')
} else {
    $genArgs = '-G "Visual Studio 17 2022" -A x64'
}

$cmdConfig = 'call "{0}" && cd /d "{1}" && "{2}" "{3}" {4}' -f $vcvars, $buildDir, $cmakeExe, $scriptDir, $genArgs
cmd.exe /c $cmdConfig
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configuration failed."; exit 1 }

# 3. Build Release
Write-Host "`n[2/2] Building Release targets (DLSS_Nuke_Worker, nvngx)..." -ForegroundColor Yellow
$cmdBuild = 'call "{0}" && cd /d "{1}" && "{2}" --build . --config Release' -f $vcvars, $buildDir, $cmakeExe
cmd.exe /c $cmdBuild
if ($LASTEXITCODE -ne 0) { Write-Error "Compilation failed."; exit 1 }

# Locate built artifacts
$candidatesExe = @(
    (Join-Path $buildDir "DLSS_Nuke_Worker.exe"),
    (Join-Path $buildDir "Release\DLSS_Nuke_Worker.exe")
)
$candidatesDll = @(
    (Join-Path $buildDir "nvngx.dll"),
    (Join-Path $buildDir "Release\nvngx.dll")
)

$builtExe = $null
foreach ($c in $candidatesExe) { if (Test-Path $c) { $builtExe = $c; break } }

$builtDll = $null
foreach ($c in $candidatesDll) { if (Test-Path $c) { $builtDll = $c; break } }

if (-not $builtExe) { Write-Error "DLSS_Nuke_Worker.exe was not built."; exit 1 }
if (-not $builtDll) { Write-Error "nvngx.dll was not built."; exit 1 }

Write-Host "===================================================" -ForegroundColor Green
Write-Host "[SUCCESS] Built Worker: $builtExe" -ForegroundColor Green
Write-Host "[SUCCESS] Built Shim:   $builtDll" -ForegroundColor Green
Write-Host "===================================================" -ForegroundColor Green

# 4. Deploy to runtime locations
$repoRuntime = Join-Path $repoRoot "runtime"
if (Test-Path $repoRuntime) {
    Copy-Item $builtExe (Join-Path $repoRuntime "DLSS_Nuke_Worker.exe") -Force
    Copy-Item $builtDll (Join-Path $repoRuntime "nvngx.dll") -Force
    Write-Host "[DEPLOYED] To repo runtime: $repoRuntime" -ForegroundColor Cyan
}

$userRuntime = "$env:USERPROFILE\.nuke\DLSS5Live\runtime"
if (Test-Path $userRuntime) {
    Copy-Item $builtExe (Join-Path $userRuntime "DLSS_Nuke_Worker.exe") -Force
    Copy-Item $builtDll (Join-Path $userRuntime "nvngx.dll") -Force
    Write-Host "[DEPLOYED] To user plugin runtime: $userRuntime" -ForegroundColor Cyan
}

Write-Host "`nAll build and deployment tasks completed successfully!" -ForegroundColor Green
