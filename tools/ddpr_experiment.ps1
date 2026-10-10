# Runs ddpr_xref.ps1 with x86\d3d9_experiment.dll standing in as the game's
# d3d9.dll: launch A saves, launch B puts the whole snapshot back. The real DLL
# goes back afterwards whatever happens; ddpr_xref puts the user's slot back.
param([int]$SaveAt = 600, [int]$LoadAt = 300, [int]$QuitA = 700, [int]$QuitB = 900, [string]$Out = 'F:\ddpr-experiment')
$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\DoDonPachi Resurrection'
$dll = Join-Path $game 'd3d9.dll'
$keep = Join-Path $game 'd3d9.dll.real'
$exp = Join-Path $PSScriptRoot '..\x86\d3d9_experiment.dll'

if (Get-Process default -ErrorAction SilentlyContinue) { throw 'default.exe is running' }
if (Test-Path $keep) { throw "$keep exists - a previous run did not put the real DLL back; check it first" }
Copy-Item $dll $keep
try {
	Copy-Item $exp $dll -Force
	& (Join-Path $PSScriptRoot 'ddpr_xref.ps1') -SaveAt $SaveAt -LoadAt $LoadAt -QuitA $QuitA -QuitB $QuitB -Out $Out
} finally {
	Get-Process default -ErrorAction SilentlyContinue | Stop-Process -Force
	Start-Sleep 2
	Copy-Item $keep $dll -Force
	Remove-Item $keep
}
