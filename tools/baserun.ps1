# How stable is the game's image base across launches?
#
# Two runs both reported image 006A0000, which would be a large result if it
# held: cross-session restore fails at the region stage because addresses move,
# and an executable that lands in the same place every launch removes the
# biggest single source of that movement. Two samples prove nothing, so this
# takes many.
#
# The expectation worth stating before measuring: Windows picks an image's ASLR
# base once per boot and reuses it for every load of that image until the next
# boot. If that is what governs here, every launch in this session agrees and a
# launch after a reboot does not - which would mean "cross-session" within one
# boot is a much smaller problem than "cross-boot", and worth separating.
#
#   tools\baserun.ps1 -N 6

param(
	[int]$N = 6,
	[int]$StartTimeoutSec = 90,
	[int]$RunTimeoutSec = 180,
	[switch]$ResetSave
)

$game = "C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi"
$trace = Join-Path $game "gh_trace.txt"
$appid = 400910
$rows = @()
$keep = Join-Path $PSScriptRoot "..\det\base"
$null = New-Item -ItemType Directory -Force -Path $keep

if (Get-Process rabiribi -EA SilentlyContinue) {
	Write-Host "  the game is already running; close it first" -ForegroundColor Red
	exit 1
}

for ($i = 1; $i -le $N; $i++) {
	Write-Host ("=== launch {0} of {1} ===" -f $i, $N)
	Remove-Item $trace -Force -EA SilentlyContinue
	if ($ResetSave -and (Test-Path (Join-Path $game 'reset-save.ps1'))) {
		& (Join-Path $game 'reset-save.ps1') | Out-Null
	}

	Start-Process "steam://rungameid/$appid"

	$p = $null
	$t0 = Get-Date
	while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt $StartTimeoutSec) {
		Start-Sleep -Milliseconds 500
		$p = Get-Process rabiribi -EA SilentlyContinue
	}
	if (-not $p) { Write-Host "  never started, skipping" -ForegroundColor Yellow; continue }
	Write-Host ("  started, pid {0}" -f $p.Id)

	# D3D9SW_QUIT_AT closes the window on its own at frame 1800. The timeout is
	# only here so a hung launch cannot stall the whole sweep.
	$t0 = Get-Date
	while ((Get-Process -Id $p.Id -EA SilentlyContinue) -and
		((Get-Date) - $t0).TotalSeconds -lt $RunTimeoutSec) {
		Start-Sleep -Seconds 1
	}
	if (Get-Process -Id $p.Id -EA SilentlyContinue) {
		Write-Host "  did not close itself, ending it" -ForegroundColor Yellow
		Stop-Process -Id $p.Id -Force
		Start-Sleep -Seconds 2
	}

	if (-not (Test-Path $trace)) { Write-Host "  no trace written" -ForegroundColor Yellow; continue }

	# Kept before parsing, not after. The first sweep deleted each trace at the
	# top of the loop and read it at the bottom; when the read failed, five
	# launches' worth of evidence was already gone and the run had to be redone.
	Copy-Item $trace (Join-Path $keep "launch$i.txt") -Force

	# One string, not three. Get-Content returns an array, and -match against an
	# array filters it and never populates $Matches - which is exactly how the
	# first sweep silently produced six empty rows.
	$h = (Get-Content $trace -TotalCount 3) -join "`n"
	$heap = if ($h -match 'heap ([0-9A-F]{8})') { $Matches[1] } else { '?' }
	$img = if ($h -match 'image ([0-9A-F]{8})') { $Matches[1] } else { '?' }
	$ops = if ($h -match '(\d+) op\(s\)') { $Matches[1] } else { '?' }
	$rows += [pscustomobject]@{ Run = $i; Image = $img; Heap = $heap; Ops = $ops }
	Write-Host ("  image {0}  heap {1}  {2} op(s)" -f $img, $heap, $ops)
}

Write-Host "`n=== every launch ==="
$rows | Format-Table -AutoSize | Out-String | Write-Host

$img = $rows.Image | Sort-Object -Unique
$heap = $rows.Heap | Sort-Object -Unique
$ops = $rows.Ops | Sort-Object -Unique
Write-Host ("  distinct image bases : {0}  ({1})" -f $img.Count, ($img -join ', '))
Write-Host ("  distinct heap bases  : {0}  ({1})" -f $heap.Count, ($heap -join ', '))
Write-Host ("  distinct op counts   : {0}  ({1})" -f $ops.Count, ($ops -join ', '))
if ($img.Count -eq 1 -and $rows.Count -gt 2) {
	Write-Host "`n  The image base did not move across $($rows.Count) launches in this boot."
	Write-Host "  Worth repeating after a reboot: if it changes there and only there,"
	Write-Host "  the base is chosen per boot and same-boot restore is the easier target."
}
