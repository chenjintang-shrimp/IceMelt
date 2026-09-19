@echo off
rem IceMelt: apply the melt -- strip the freeze filters, write the SYSTEM hive back to the
rem raw disk (plus its RegBack copy) and mark the volume dirty. Nothing is reset
rem automatically: reboot the machine by hand once the writes verify.
rem Irreversible on this machine (only a VM snapshot can undo it).
rem Needs an elevated prompt: right-click the file, pick "Run as administrator".
"%~dp0icemelt.exe" --melt --yes-i-know %*
echo.
pause
