@echo off
cd /d "%~dp0"

echo Stopping any running instance of debounce.exe...
taskkill /f /im debounce.exe >nul 2>&1

echo Compiling debounce.c...
"tcc\tcc.exe" debounce.c -luser32 -Wl,-subsystem=windows -o debounce.exe

if %errorlevel% equ 0 (
    echo.
    echo [SUCCESS] debounce.exe created in the main folder!
) else (
    echo.
    echo [ERROR] Compilation failed!
)

pause