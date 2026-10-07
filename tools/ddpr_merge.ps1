param([string]$Save = 'F:\ddpr-play\session01', [int]$LoadAt = 600, [string]$Out = 'F:\ddpr-merge',
	[string]$Take = 'dat112.bin+328000,default.exe+111000,D000000:800000,F000000:1000000,10000000:3000000,14000000:4B400000,5F400000:800000,78000000:6000000,self',
	[string]$Sites = '6830,732D', [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\DoDonPachi Resurrection'
$cfg = Join-Path $game 'd3d9_sw.cfg'
$log = Join-Path $game 'd3d9_sw_savestate_default.txt'
if (Get-Process default -ErrorAction SilentlyContinue) { throw 'default.exe is running' }

$dir = Join-Path $Out (Get-Date -Format 'MMdd_HHmmss')
$keep = Join-Path $dir 'user_slot0_backup'
New-Item -ItemType Directory -Force $keep | Out-Null
Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $keep
Copy-Item $cfg (Join-Path $dir 'd3d9_sw.cfg.user') -Force
$orig = Get-Content $cfg -Raw

try {
	Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Remove-Item
	Get-ChildItem $Save -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $game
	Set-Content $cfg ($orig.TrimEnd() + "`r`n# ddpr_merge run`r`nD3D9SW_GHPLACE=1`r`nD3D9SW_SLOTFILE=1`r`nD3D9SW_MERGE=1`r`nD3D9SW_MERGE_TAKE=$Take`r`nD3D9SW_MERGE_SITES=$Sites`r`nD3D9SW_LOAD_AT=$LoadAt`r`n" + (($Extra | ForEach-Object { "$_`r`n" }) -join '')) -NoNewline
	$logAt = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
	Start-Process 'steam://rungameid/464450'
	$p = $null; $t0 = Get-Date
	while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt 120) { Start-Sleep -Milliseconds 200; $p = Get-Process default -ErrorAction SilentlyContinue | Select-Object -First 1 }
	if (-not $p) { throw 'game never started' }
	"pid $($p.Id) - merge load at frame $LoadAt (~$([math]::Round($LoadAt / 60)) s); close the game when done"
	$p.WaitForExit()
	Start-Sleep 2
	if (Test-Path $log) {
		$fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $fs.Seek($logAt, 'Begin') | Out-Null
		$sr = New-Object IO.StreamReader($fs); Set-Content (Join-Path $dir 'savestate.txt') $sr.ReadToEnd(); $sr.Close()
	}
	Get-ChildItem $game -Filter 'gh_*.txt' | Copy-Item -Destination $dir
	"log: $dir\savestate.txt"
} finally {
	Set-Content $cfg $orig -NoNewline
	Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Remove-Item
	Get-ChildItem $keep -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $game -Force
}
