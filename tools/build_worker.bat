@echo off
setlocal
call "%~dp0..\package\tools\msvc\vcvars64.bat"
cd /d "%~dp0..\worker\build"
"%~dp0..\package\tools\cmake\bin\cmake.exe" "%~dp0..\worker" -G Ninja -DCMAKE_MAKE_PROGRAM="%~dp0..\package\tools\ninja\ninja.exe" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%
"%~dp0..\package\tools\cmake\bin\cmake.exe" --build .
if errorlevel 1 exit /b %errorlevel%
copy /Y "%~dp0..\worker\build\DLSS_Nuke_Worker.exe" "%~dp0..\runtime\DLSS_Nuke_Worker.exe"
echo [SUCCESS] Worker build and copy completed.
