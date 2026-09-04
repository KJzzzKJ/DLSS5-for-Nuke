@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
title DLSS 5 for Nuke - Installer

echo ===================================================
echo   DLSS 5 for Foundry Nuke (Native NDK Plugin)
echo   Automated Installer
echo ===================================================
echo.

set "NUKE_USER_DIR=%USERPROFILE%\.nuke"
set "PLUGIN_DIR=%NUKE_USER_DIR%\DLSS5Live"
set "RUNTIME_TARGET=%PLUGIN_DIR%\runtime"

if not exist "%NUKE_USER_DIR%" (
    echo [*] Creating Nuke user directory: %NUKE_USER_DIR%
    mkdir "%NUKE_USER_DIR%"
)

if not exist "%PLUGIN_DIR%" (
    echo [*] Creating DLSS 5 plugin directory: %PLUGIN_DIR%
    mkdir "%PLUGIN_DIR%"
)

if not exist "%RUNTIME_TARGET%" (
    echo [*] Creating runtime directory: %RUNTIME_TARGET%
    mkdir "%RUNTIME_TARGET%"
)

REM 1. Clean up flat root DLL to prevent ABI conflict
if exist "%PLUGIN_DIR%\DLSS5Live.dll" del /Q /F "%PLUGIN_DIR%\DLSS5Live.dll" >nul 2>&1
if exist "%NUKE_USER_DIR%\DLSS5Live.dll" del /Q /F "%NUKE_USER_DIR%\DLSS5Live.dll" >nul 2>&1

REM 2. Install Versioned Binaries (Nuke 15 & Nuke 17)
set "PKG_ROOT=%~dp0"
if not exist "%PKG_ROOT%bin" (
    if exist "%~dp0..\bin" set "PKG_ROOT=%~dp0..\"
)

if not exist "%PLUGIN_DIR%\bin\Nuke15" mkdir "%PLUGIN_DIR%\bin\Nuke15"
if not exist "%PLUGIN_DIR%\bin\Nuke17" mkdir "%PLUGIN_DIR%\bin\Nuke17"

if exist "%PKG_ROOT%bin\Nuke15\DLSS5Live.dll" (
    copy /Y "%PKG_ROOT%bin\Nuke15\DLSS5Live.dll" "%PLUGIN_DIR%\bin\Nuke15\DLSS5Live.dll" >nul
    echo [*] Installed Nuke 15 DLL
)
if exist "%PKG_ROOT%bin\Nuke17\DLSS5Live.dll" (
    copy /Y "%PKG_ROOT%bin\Nuke17\DLSS5Live.dll" "%PLUGIN_DIR%\bin\Nuke17\DLSS5Live.dll" >nul
    echo [*] Installed Nuke 17 DLL
)

REM Copy dynamic routing init.py, menu.py, and icons
if exist "%PKG_ROOT%init.py" copy /Y "%PKG_ROOT%init.py" "%PLUGIN_DIR%\init.py" >nul
if exist "%~dp0init.py" copy /Y "%~dp0init.py" "%PLUGIN_DIR%\init.py" >nul
if exist "%PKG_ROOT%menu.py" copy /Y "%PKG_ROOT%menu.py" "%PLUGIN_DIR%\menu.py" >nul
if exist "%~dp0menu.py" copy /Y "%~dp0menu.py" "%PLUGIN_DIR%\menu.py" >nul
if exist "%PKG_ROOT%DLSS5.png" copy /Y "%PKG_ROOT%DLSS5.png" "%PLUGIN_DIR%\DLSS5.png" >nul
if exist "%~dp0DLSS5.png" copy /Y "%~dp0DLSS5.png" "%PLUGIN_DIR%\DLSS5.png" >nul

REM 2. Copy Runtime directory
set "SOURCE_RUNTIME="
if exist "%~dp0runtime" (
    set "SOURCE_RUNTIME=%~dp0runtime"
) else if exist "%~dp0..\runtime" (
    set "SOURCE_RUNTIME=%~dp0..\runtime"
)

if not "%SOURCE_RUNTIME%"=="" (
    echo [*] Deploying DLSS 5 neural runtime to: %RUNTIME_TARGET%
    xcopy /Y /E /I /Q "%SOURCE_RUNTIME%\*.*" "%RUNTIME_TARGET%\" >nul
    rem Remove temporary log files if any were copied
    if exist "%RUNTIME_TARGET%\ReShade.log" del /Q "%RUNTIME_TARGET%\ReShade.log*" >nul 2>&1
    if exist "%RUNTIME_TARGET%\dlss5-feed-host.log" del /Q "%RUNTIME_TARGET%\dlss5-feed-host.log" >nul 2>&1
) else (
    echo [WARNING] runtime/ folder not found. Please ensure runtime components are deployed to %RUNTIME_TARGET%
)

REM Copy README to plugin dir for self-contained package
if exist "%~dp0README.md" copy /Y "%~dp0README.md" "%PLUGIN_DIR%\README.md" >nul
if exist "%PKG_ROOT%README.md" copy /Y "%PKG_ROOT%README.md" "%PLUGIN_DIR%\README.md" >nul

REM 3. Register search path in init.py (Append-only: never overwrites existing init.py)
set "INIT_FILE=%NUKE_USER_DIR%\init.py"
if exist "%INIT_FILE%" (
    findstr /I "DLSS5Live" "%INIT_FILE%" >nul 2>&1
    if !errorlevel! equ 0 (
        set "ALREADY_REGISTERED=1"
    )
)

if not defined ALREADY_REGISTERED (
    echo [*] Registering DLSS5 plugin search path in %INIT_FILE%...
    (
        echo.
        echo # --- DLSS 5 Native Live ---
        echo import os, nuke
        echo _dlss_p = os.path.expanduser('~/.nuke/DLSS5Live'^).replace('\\', '/'^)
        echo if os.path.isdir(_dlss_p^) and _dlss_p not in nuke.pluginPath(^):
        echo     nuke.pluginAddPath(_dlss_p^)
    ) >> "%INIT_FILE%"
) else (
    echo [*] Plugin path entry already exists in %INIT_FILE%. (Preserved unchanged)
)

echo.
echo ===================================================
echo [SUCCESS] Installation completed successfully!
echo.
echo Installed location:
echo   - Isolated Folder: %PLUGIN_DIR%
echo     [+] bin\Nuke15\DLSS5Live.dll
echo     [+] bin\Nuke17\DLSS5Live.dll
echo     [+] runtime\
echo     [+] init.py
echo     [+] menu.py
echo     [+] DLSS5.png
echo     [+] README.md
echo.
echo Configuration:
echo   - Plugin Path: %INIT_FILE% (Appended cleanly; preserved existing scripts)
echo   - Node Menu:   %PLUGIN_DIR%\menu.py (Auto-loaded via pluginPath)
echo   - User menu.py: Untouched (Zero overwrite risk)
echo.
echo You can now launch Foundry Nuke and press Tab -^> "DLSS5Live".
echo ===================================================
echo.
pause
