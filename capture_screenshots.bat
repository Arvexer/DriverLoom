@echo off
setlocal
cd /d "%~dp0"

if not exist "build\DriverLoom.exe" (
    echo [ERROR] build\DriverLoom.exe not found. Build the project first:
    echo   cmake --build build -j 8
    pause
    exit /b 1
)

if not exist "docs\screenshots" (
    mkdir "docs\screenshots"
)

echo [1/2] Capturing DriverLoom screenshots across all tabs...
"build\DriverLoom.exe" --lang ru --capture-suite "docs\screenshots"
if errorlevel 1 (
    echo [ERROR] DriverLoom screenshot capture failed.
    pause
    exit /b 1
)

echo [2/2] Converting BMP screenshots to compressed PNG...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Add-Type -AssemblyName System.Drawing; Get-ChildItem -Path 'docs\screenshots' -Filter '*.bmp' | ForEach-Object { $png = [System.IO.Path]::ChangeExtension($_.FullName, '.png'); $bmp = [System.Drawing.Bitmap]::new($_.FullName); $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); Remove-Item $_.FullName -Force }"

if errorlevel 1 (
    echo [ERROR] PNG conversion failed.
    pause
    exit /b 1
)

echo.
echo [OK] Screenshots saved to docs\screenshots\:
dir /b "docs\screenshots\*.png"
echo.
pause
