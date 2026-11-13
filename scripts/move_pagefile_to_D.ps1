# Moves the Windows pagefile from C: (3 GB free, cannot grow) to D: with a fixed 16-32 GB size.
# Fixes Victoria 3 crashing on "New Game" from exhausted commit memory. Needs admin + reboot.
# Undo: System > Advanced system settings > Performance > Advanced > Virtual memory >
#       tick "Automatically manage paging file size for all drives".
$ErrorActionPreference = 'Stop'
$log = 'D:\SteamLibrary\steamapps\common\Victoria 3\v3x\scripts\move_pagefile.log'
try {
    $cs = Get-CimInstance Win32_ComputerSystem
    Set-CimInstance -InputObject $cs -Property @{ AutomaticManagedPagefile = $false }
    Get-CimInstance Win32_PageFileSetting | Remove-CimInstance
    New-CimInstance -ClassName Win32_PageFileSetting -Property @{ Name = 'D:\pagefile.sys'; InitialSize = [uint32]16384; MaximumSize = [uint32]32768 } | Out-Null
    $s = Get-CimInstance Win32_PageFileSetting | ForEach-Object { "$($_.Name) $($_.InitialSize)-$($_.MaximumSize)MB" }
    "OK auto=$((Get-CimInstance Win32_ComputerSystem).AutomaticManagedPagefile) settings: $s (reboot required)" | Out-File $log
} catch { "ERROR $_" | Out-File $log }
