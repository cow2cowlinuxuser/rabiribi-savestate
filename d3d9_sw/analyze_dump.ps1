# Reopen a crash dump with Unity's symbols and dump the standard triage to a file.
#
# Separate from catch_failfast.ps1 because the two jobs have different failure
# modes: that one has to attach to a live game without killing it, this one only
# has to read a file, and mixing them made the live path riskier than it needed
# to be.
#
# Usage: .\analyze_dump.ps1 [-Dump failfast.dmp]

param([string]$Dump = 'failfast.dmp')

$ErrorActionPreference = 'Stop'

$cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
if (-not (Test-Path $cdb)) { throw "cdb not found at $cdb" }

if (-not [System.IO.Path]::IsPathRooted($Dump)) { $Dump = Join-Path $PSScriptRoot $Dump }
if (-not (Test-Path $Dump)) { throw "dump not found at $Dump" }

$script = Join-Path $PSScriptRoot 'analyze_dump.cdb'
if (-not (Test-Path $script)) { throw "command script not found at $script" }

$out = Join-Path $PSScriptRoot 'analysis.txt'
if (Test-Path $out) { Remove-Item $out -Force }

New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'symbols-cache') | Out-Null

$mb = [math]::Round((Get-Item $Dump).Length / 1MB, 1)
Write-Host "Opening $Dump ($mb MB). Transcript: $out"
Write-Host 'First run may pause while symbols load. The session stays interactive afterwards.'

# -z    open a dump rather than attach to a process.
# -cf   run the script file; see the note in catch_failfast.ps1 on why not -c.
# -logo overwrite, so one analysis is one file.
& $cdb -z $Dump -cf $script -logo $out
