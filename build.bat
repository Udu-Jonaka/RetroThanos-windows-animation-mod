@echo off
cd /d "%~dp0"
echo ==============================================
echo Building Retro TV & Thanos Animations
echo ==============================================

echo [1/2] Compiling RetroThanosEngine.dll (Static runtime)...
"C:\Program Files\Windhawk\Compiler\bin\clang++.exe" -target x86_64-w64-mingw32 -O2 -static -shared RetroThanosEngine.cpp -ldwmapi -lgdi32 -lcomctl32 -luser32 -o RetroThanosEngine.dll
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile RetroThanosEngine.dll
    pause
    exit /b %errorlevel%
)

echo [2/2] Compiling RetroThanosApp.exe (Static runtime)...
"C:\Program Files\Windhawk\Compiler\bin\clang++.exe" -target x86_64-w64-mingw32 -O2 -static -mwindows RetroThanosApp.cpp -lshell32 -luser32 -o RetroThanosApp.exe
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile RetroThanosApp.exe
    pause
    exit /b %errorlevel%
)

echo.
echo ==============================================
echo SUCCESS: RetroThanosApp.exe is ready!
echo ==============================================
pause
