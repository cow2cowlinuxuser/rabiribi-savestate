# Played sessions: launch through Steam, one save at 1:00 (in frames, at 60 fps),
# close at 1:10. A countdown with beeps before each launch.
param([int]$Sessions = 1, [string]$Out = 'F:\ddpr-play', [int]$Gap = 15,
      [string]$SaveAt = '3600', [int]$QuitAt = 4200)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
for ($s = 1; $s -le $Sessions; $s++) {
	$dest = Join-Path $Out ('session{0:D2}' -f $s)
	if (Test-Path $dest) { throw "$dest exists" }
	"session $s of $Sessions starts in $Gap s"
	for ($t = $Gap; $t -gt 0; $t--) { if ($t -le 3) { [console]::Beep(880, 150) }; Start-Sleep 1 }
	[console]::Beep(1320, 300)
	& (Join-Path $PSScriptRoot 'ddpr_baseline.ps1') -Runs 1 -SaveAt $SaveAt -QuitAt $QuitAt -Out $Out -Extra 'D3D9SW_GHPLACE=1'
	Rename-Item (Join-Path $Out 'run01') $dest
	$n = @(Get-ChildItem $dest -Directory -Filter 'save*').Count + [int](Test-Path (Join-Path $dest 'd3d9sw_slot0.bin'))
	"session $s done: $n save(s) in $dest"
	[console]::Beep(660, 400)
}
