@echo off
rem IceMelt: pre-melt root-cause scan. Read-only: no SYSTEM/RegBack write, no reset ---
rem it still loads the WinDisk driver and does raw I/O probes, so it needs an elevated
rem prompt: right-click the file, pick "Run as administrator".
rem Extra arguments are passed through to icemelt.exe.
"%~dp0icemelt.exe" --preflight %*
echo.
pause
