@echo off
REM Run the actual native x64 Explorer/XAML TAP stress suite in the interactive guest.
start "" /min powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\OEM\win11_native_x64_stress.ps1"
exit /b 0
