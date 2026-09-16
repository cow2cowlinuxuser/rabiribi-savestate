# The cross-session restore test: write a state in one launch, read it in the
# next, and find out exactly which stage refuses first.
#
# Everything this depends on is now measured rather than hoped for. The game's
# allocation stream is byte-identical across launches, reboots and machines; the
# heap is pinned at 50000000; our four wrappers are pinned at 60000000-63000000;
# and within one boot rabiribi.exe does not move. So nothing in the address
# space is *known* to differ between these two launches - which is precisely why
# it is worth running, because whatever refuses will be something we did not
# know about.
#
# Both launches reset the save file and take no input, so a difference cannot be
# the operator. Keep this in one boot: across a reboot the executable moves and
# the answer would be about that instead.
#
#   tools\xrestore.ps1

param([int]$Frame = 1200, [int]$Quit = 1800)

$game = "C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi"
$cfg = Join-Path $game "d3d9_sw.cfg"
$log = Join-Path $game "d3d9_sw_savestate_rabiribi.txt"
$out = Join-Path $PSScriptRoot "..\det\xrestore"
$null = New-Item -ItemType Directory -Force -Path $out

function SetKnobs([hashtable]$kv) {
	$lines = [IO.File]::ReadAllText($cfg) -split "`r?`n"
	foreach ($k in $kv.Keys) {
		if ($lines -match "^$k=") { $lines = $lines | % { if ($_ -match "^$k=") { "$k=$($kv[$k])" } else { $_ } } }
		else { $lines += "$k=$($kv[$k])" }
	}
	[IO.File]::WriteAllText($cfg, (($lines -join "`r`n").TrimEnd() + "`r`n"))
}

function RunOnce([string]$label) {
	if (Get-Process rabiribi -EA SilentlyContinue) { Write-Host "  game running"; exit 1 }
	Remove-Item $log -Force -EA SilentlyContinue
	& (Join-Path $game 'reset-save.ps1') | Out-Null

	Start-Process "steam://rungameid/400910"
	$p = $null; $t0 = Get-Date
	while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt 90) {
		Start-Sleep -Milliseconds 500; $p = Get-Process rabiribi -EA SilentlyContinue
	}
	if (-not $p) { Write-Host "  never started" -ForegroundColor Red; return $null }
	$t0 = Get-Date
	while ((Get-Process -Id $p.Id -EA SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -lt 240) {
		Start-Sleep -Seconds 1
	}
	if (Get-Process -Id $p.Id -EA SilentlyContinue) { Stop-Process -Id $p.Id -Force; Start-Sleep 2 }
	$dst = Join-Path $out "$label.txt"
	if (Test-Path $log) { Copy-Item $log $dst -Force; return $dst }
	return $null
}

# --- launch 1: take the state, leave it in a file -------------------------
Write-Host "=== launch 1: save at frame $Frame, quit at $Quit ==="
SetKnobs @{ 'D3D9SW_SLOTFILE' = 1; 'D3D9SW_SAVE_AT' = $Frame; 'D3D9SW_LOAD_AT' = 0; 'D3D9SW_QUIT_AT' = $Quit }
$l1 = RunOnce "launch1-save"
$slot = Get-ChildItem $game -Filter 'd3d9sw_slot*.bin' -EA SilentlyContinue
if ($slot) { $slot | % { Write-Host ("  slot file: {0}  {1:N0} bytes" -f $_.Name, $_.Length) } }
else { Write-Host "  no slot file was written - the rest of this test is meaningless" -ForegroundColor Red; exit 1 }

# --- launch 2: put it back -----------------------------------------------
Write-Host "`n=== launch 2: restore at frame $Frame, quit at $Quit ==="
SetKnobs @{ 'D3D9SW_SAVE_AT' = 0; 'D3D9SW_LOAD_AT' = $Frame }
$l2 = RunOnce "launch2-restore"

# --- what happened --------------------------------------------------------
Write-Host "`n=== what the restore said ==="
if (-not $l2) { Write-Host "  no log from launch 2" -ForegroundColor Red; exit 1 }
$c = Get-Content $l2
$c | Select-String -Pattern 'load-at:|REFUSED|refus|abort|mismatch|cannot|excluded now but was saved|restore|slotfile|region stage' |
	Select-Object -First 40 | % { Write-Host "  $($_.Line.Trim())" }

Write-Host "`n=== and whether it survived ==="
$c | Select-String -Pattern 'quit-at: frame|exception|fault|C0000005' | Select-Object -First 6 | % { Write-Host "  $($_.Line.Trim())" }
Write-Host "`n  logs in det\xrestore\"
