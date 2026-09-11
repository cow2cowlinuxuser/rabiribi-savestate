# Capture the silent death after a restore.
#
# The process dies with nothing in our log, which is the signature of __fastfail:
# it issues int 0x29, and that exception is delivered only to a debugger. Our
# vectored handler never runs, and neither do the TerminateProcess or abort
# hooks, so the only way to name the faulting code is to be the debugger.
#
# Attaches rather than launches, because launching the game from a shell makes it
# relaunch itself and we would lose the child. Start the game from Steam first,
# then run this, then do the repro.

# Takes the process name because this stopped being an OSFE-only tool the moment
# a second host started dying: Rabbit and Steel ends a restore with "load:" as
# its last log line and nothing after it, which is this exact signature.
param([string]$Exe = 'OSFE')

$ErrorActionPreference = 'Stop'

$cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
if (-not (Test-Path $cdb)) { throw "cdb not found at $cdb" }

$script = Join-Path $PSScriptRoot 'catch_failfast.cdb'
if (-not (Test-Path $script)) { throw "command script not found at $script" }

$proc = Get-Process -Name $Exe -ErrorAction SilentlyContinue
if (-not $proc) { throw "$Exe is not running. Start it from Steam first, then run this." }
if ($proc -is [array]) { $proc = $proc | Sort-Object StartTime -Descending | Select-Object -First 1 }

$out = Join-Path $PSScriptRoot 'failfast.txt'
if (Test-Path $out) { Remove-Item $out -Force }

# Unity's symbols turn a column of UnityPlayer+hex into function names. The
# expanded PDB should already be here; the cache directory is for anything else
# the symbol server is asked for during the session.
$pdb = Join-Path $PSScriptRoot 'symbols\UnityPlayer_Win64_mono_x64.pdb'
if ($Exe -eq 'OSFE' -and -not (Test-Path $pdb)) {
    Write-Warning "UnityPlayer PDB missing at $pdb - the stack will be raw offsets."
    Write-Warning 'Fetch it from symbolserver.unity3d.com and expand with %WINDIR%\System32\expand.exe.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'symbols-cache') | Out-Null

Write-Host "Attaching to $Exe pid $($proc.Id). Log: $out"
Write-Host 'The game will freeze for a moment while symbols load, then resume.'
Write-Host 'Do the repro: savestate, reload, then move.'
Write-Host 'When the game dies, look for ===FAILFAST=== in the log.'

# -p    attach to this pid.
# -pd   detach on debugger exit instead of killing the game. Without this,
#       quitting cdb - or cdb failing during startup, which is how the first
#       attempt at this ended - takes the game down with it.
# -cf   run this script file. Not -c: the commands contain quotes, and a quoted
#       argument containing quotes does not survive the shell.
# -logo overwrite the log rather than append, so one run is one file.
& $cdb -p $proc.Id -pd -cf $script -logo $out
