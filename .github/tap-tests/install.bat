@echo off
REM Launch outside SetupComplete so Explorer can initialize independently.
start "" /min powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\OEM\win11_native_x64_guest_probe.ps1"
exit /b 0
