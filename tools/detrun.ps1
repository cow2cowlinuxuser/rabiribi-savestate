# Two runs that differ only by being two runs.
#
# Akaginite, who built a TAS environment for this game, found that the game's
# random number consumption changes with two things: whether an XInput pad is
# recognised, and the save data read just before. Both feed the allocation
# stream, which is why our traces have never matched run to run. The pad is
# pinned by xinput1_4.dll; the save data is pinned here, by resetting it to the
# same bytes before every run.
#
# Usage:
#   tools\detrun.ps1 1          prepare run 1, then launch the game
#   tools\detrun.ps1 1 -Collect after the game exits, file the evidence
#   tools\detrun.ps1 2          ... and again
#   tools\detrun.ps1 -Compare   diff the two by block name

param(
	[Parameter(Position = 0)][int]$Run,
	[switch]$Collect,
	[switch]$Compare
)

$game = "C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi"
$out = Join-Path $PSScriptRoot "..\det"
$null = New-Item -ItemType Directory -Force -Path $out

function Die($m) { Write-Host "  $m" -ForegroundColor Red; exit 1 }

if ($Compare) {
	$a = Join-Path $out "run1\gh_trace.txt"
	$b = Join-Path $out "run2\gh_trace.txt"
	if (-not (Test-Path $a) -or -not (Test-Path $b)) { Die "need both runs collected first" }
	Write-Host "=== two runs, compared by block name (site + ordinal) ==="
	& py -3 (Join-Path $PSScriptRoot "tagdiff.py") $a $b
	Write-Host "`n=== which audio tracks each run loaded ==="
	foreach ($r in 1, 2) {
		$log = Join-Path $out "run$r\savestate.txt"
		if (Test-Path $log) {
			$v = Select-String -Path $log -Pattern 'vorbis:' | Select-Object -First 4
			Write-Host "  run ${r}: $($v.Count) vorbis line(s)"
			$v | ForEach-Object { Write-Host "    $($_.Line.Trim())" }
		}
	}
	exit 0
}

if (-not $Run) { Die "say which run: tools\detrun.ps1 1" }
$dir = Join-Path $out "run$Run"

if ($Collect) {
	$null = New-Item -ItemType Directory -Force -Path $dir
	$got = 0
	foreach ($f in 'gh_trace.txt', 'd3d9_sw_savestate_rabiribi.txt', 'xinput_sw.log') {
		$p = Join-Path $game $f
		if (Test-Path $p) {
			$name = if ($f -like 'd3d9_sw_save*') { 'savestate.txt' } else { $f }
			Copy-Item $p (Join-Path $dir $name) -Force
			$got++
		}
	}
	Write-Host "=== run $Run filed: $got file(s) into det\run$Run ==="
	$t = Join-Path $dir "gh_trace.txt"
	if (Test-Path $t) {
		$n = (Get-Content $t | Measure-Object -Line).Lines
		Write-Host "  gh_trace.txt  $n line(s)"
	}
	$x = Join-Path $dir "xinput_sw.log"
	if (Test-Path $x) { Get-Content $x | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" } }
	else { Write-Host "  no xinput_sw.log - the stub was never loaded, the pad is NOT pinned" -ForegroundColor Yellow }
	exit 0
}

if (Get-Process rabiribi -ErrorAction SilentlyContinue) { Die "the game is running; close it first" }

# A trace left from last time would be collected as this run's if the game
# failed to write one, and a silent stale file is the worst kind of evidence.
Get-ChildItem $game -Filter 'gh_trace*.txt' -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item (Join-Path $game 'xinput_sw.log') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $game 'd3d9_sw_savestate_rabiribi.txt') -Force -ErrorAction SilentlyContinue

$reset = Join-Path $game "reset-save.ps1"
if (Test-Path $reset) { & $reset | Out-Null; Write-Host "  save data reset" }
else { Write-Host "  reset-save.ps1 missing - save data is NOT pinned" -ForegroundColor Yellow }

Write-Host "=== run $Run prepared ==="
Write-Host "  launch the game, touch nothing, and let it sit."
Write-Host "  the save fires by itself at frame 1200 and writes the trace."
Write-Host "  then quit and run: tools\detrun.ps1 $Run -Collect"
