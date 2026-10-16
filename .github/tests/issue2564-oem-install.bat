@echo off
start "" /min powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\OEM\issue2564-guest.ps1"
exit /b 0
