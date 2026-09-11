# Make Windows keep a record when the game dies.
#
#   .\crashlogs.ps1          look only, no admin needed, changes nothing
#   .\crashlogs.ps1 -Fix     apply the fixes, must be an Administrator window
#
# Windows already records every crash in two places, and on a healthy machine
# both work with no setup at all. The trouble is that error reporting is one of
# the first things the various "debloat" and privacy scripts turn off, and once
# it is off it fails silently - the Application Error entry still appears in the
# event log, so everything looks fine, while the archive that actually holds the
# faulting module and the loaded module list is never written. That is also why
# asking for crash dumps can appear to do nothing: dumps are handled by the same
# service, so disabling reporting disables those too.
#
# Run it with no arguments and send the output. That alone says whether there is
# anything to fix.

param([switch]$Fix)

$ErrorActionPreference = 'Stop'

$exe = 'default.exe'
$dumpFolder = 'C:\dumps'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if ($Fix -and -not $admin) {
    Write-Host ''
    Write-Host 'The -Fix switch needs an Administrator window.' -ForegroundColor Yellow
    Write-Host 'Right-click the Start button, pick "Terminal (Admin)" or'
    Write-Host '"Windows PowerShell (Admin)", then run this again.'
    Write-Host ''
    exit 1
}

$problems = @()

function Note($ok, $text) {
    if ($ok) { Write-Host "  ok    $text" -ForegroundColor Green }
    else { Write-Host "  PROBLEM  $text" -ForegroundColor Red }
}

Write-Host ''
Write-Host '--- what is switched off ---'

# Any of these four set to 1 stops reporting. The policy keys usually mean a
# group policy or a tweaking script; the plain ones are the user-facing setting.
$switches = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting',
    'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Windows Error Reporting',
    'HKCU:\Software\Microsoft\Windows\Windows Error Reporting',
    'HKCU:\Software\Policies\Microsoft\Windows\Windows Error Reporting'
)
foreach ($k in $switches) {
    $v = $null
    if (Test-Path $k) { $v = (Get-ItemProperty $k -ErrorAction SilentlyContinue).Disabled }
    if ($v -eq 1) {
        Note $false "error reporting disabled at $k"
        $problems += @{ Kind = 'switch'; Key = $k }
    } else {
        Note $true "$k"
    }
}

# The service is started on demand, so Stopped is normal and expected. Only a
# start type of Disabled actually prevents anything.
$svc = Get-Service WerSvc -ErrorAction SilentlyContinue
if (-not $svc) {
    Note $false 'the Windows Error Reporting service is not installed'
} elseif ($svc.StartType -eq 'Disabled') {
    Note $false "WerSvc start type is Disabled (should be Manual; it is currently $($svc.Status), which is fine)"
    $problems += @{ Kind = 'service' }
} else {
    Note $true "WerSvc start type is $($svc.StartType), currently $($svc.Status)"
}

Write-Host ''
Write-Host '--- locale, which differs between the machines we are comparing ---'

# This game predates Unicode as a default and reads its own paths and data
# through the ANSI codepage, so which codepage that is changes its behaviour.
# 932 is Japanese and is arguably this game's native setting; 1252 is Western.
# The one that causes trouble is 65001, the "Beta: Use Unicode UTF-8 for
# worldwide language support" checkbox, which reliably breaks software of this
# era and is the single setting worth knowing about here.
$nls = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage' -ErrorAction SilentlyContinue
$acp = $nls.ACP
"  ANSI codepage (ACP) : {0}" -f $acp
"  OEM codepage (OEMCP): {0}" -f $nls.OEMCP
if ($acp -eq '65001') {
    Note $false 'UTF-8 beta is ON. This breaks many games of this age; worth turning off as a test.'
} else {
    Note $true "not the UTF-8 beta"
}
"  system locale       : {0}" -f (Get-WinSystemLocale).Name
"  input languages     : {0}" -f ((Get-WinUserLanguageList | ForEach-Object { $_.LanguageTag }) -join ', ')

Write-Host ''
Write-Host '--- what has been recorded so far ---'

foreach ($p in @(
        "$env:ProgramData\Microsoft\Windows\WER\ReportArchive",
        "$env:ProgramData\Microsoft\Windows\WER\ReportQueue",
        "$env:LOCALAPPDATA\Microsoft\Windows\WER\ReportArchive",
        "$env:LOCALAPPDATA\Microsoft\Windows\WER\ReportQueue",
        $dumpFolder)) {
    if (Test-Path $p) {
        $all = @(Get-ChildItem $p -ErrorAction SilentlyContinue)
        $mine = @($all | Where-Object { $_.Name -match [regex]::Escape($exe) })
        '  {0,5} entries, {1,3} for {2}   {3}' -f $all.Count, $mine.Count, $exe, $p
    } else {
        '  absent                          {0}' -f $p
    }
}

# Present or not, this is where the dump setting lives, and its absence is the
# usual reason no dump ever appears even when reporting itself is healthy.
$ld = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$exe"
if (Test-Path $ld) {
    $p = Get-ItemProperty $ld
    Note $true "dumps configured: $($p.DumpFolder), type $($p.DumpType)"
} else {
    Note $false "no dump setting for $exe"
    $problems += @{ Kind = 'dumps' }
}

Write-Host ''
if ($problems.Count -eq 0) {
    Write-Host 'Nothing to fix. Crashes should already be recorded.' -ForegroundColor Green
    Write-Host "Send the newest folder from the ReportArchive above, and any .dmp from $dumpFolder."
    Write-Host ''
    exit 0
}

if (-not $Fix) {
    Write-Host "$($problems.Count) thing(s) to fix. Re-run this in an Administrator" -ForegroundColor Yellow
    Write-Host 'window with -Fix on the end:' -ForegroundColor Yellow
    Write-Host ''
    Write-Host '    .\crashlogs.ps1 -Fix'
    Write-Host ''
    exit 0
}

Write-Host '--- fixing ---'

foreach ($p in $problems) {
    switch ($p.Kind) {
        'switch' {
            Remove-ItemProperty -Path $p.Key -Name Disabled -Force -ErrorAction SilentlyContinue
            Write-Host "  re-enabled reporting at $($p.Key)"
        }
        'service' {
            Set-Service WerSvc -StartupType Manual
            Write-Host '  WerSvc set back to Manual'
        }
        'dumps' {
            New-Item -Path $dumpFolder -ItemType Directory -Force | Out-Null
            New-Item -Path $ld -Force | Out-Null
            New-ItemProperty -Path $ld -Name DumpFolder -Value $dumpFolder -PropertyType ExpandString -Force | Out-Null
            New-ItemProperty -Path $ld -Name DumpCount -Value 10 -PropertyType DWord -Force | Out-Null
            # Custom, rather than a full 1.5 GB image of this game. The flags ask
            # for stacks and registers plus module data, whatever the stacks point
            # at, and handle and thread detail - enough that an object a faulting
            # call was reaching through can be read rather than showing as
            # question marks. Tens of megabytes instead of gigabytes.
            New-ItemProperty -Path $ld -Name DumpType -Value 0 -PropertyType DWord -Force | Out-Null
            New-ItemProperty -Path $ld -Name CustomDumpFlags -Value 0x1065 -PropertyType DWord -Force | Out-Null
            Write-Host "  dumps for $exe -> $dumpFolder"
        }
    }
}

# Lets a debugger name the Windows frames later instead of leaving raw offsets.
[Environment]::SetEnvironmentVariable(
    '_NT_SYMBOL_PATH',
    'srv*C:\symbols*https://msdl.microsoft.com/download/symbols',
    'Machine')

Write-Host ''
Write-Host 'Done. Play until it dies, then send:' -ForegroundColor Green
Write-Host "  - the newest .dmp from $dumpFolder"
Write-Host '  - the newest AppCrash_default.exe_* folder from'
Write-Host '    C:\ProgramData\Microsoft\Windows\WER\ReportArchive'
Write-Host '  - d3d9_sw_savestate.txt from the game folder'
Write-Host ''
