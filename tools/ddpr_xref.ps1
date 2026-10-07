param([int]$SaveAt = 600, [int]$LoadAt = 300, [int]$QuitA = 700, [int]$QuitB = 900, [string]$Out = 'F:\ddpr-xref', [switch]$LoadOnly)
$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\DoDonPachi Resurrection'
$cfg = Join-Path $game 'd3d9_sw.cfg'
$logs = Join-Path $game 'logs'
New-Item -ItemType Directory -Force $Out | Out-Null

if (Get-Process default -ErrorAction SilentlyContinue) { throw 'default.exe is running' }
$orig = Get-Content $cfg -Raw
if ($orig -match '# ddpr_(baseline|xref) run') { throw 'd3d9_sw.cfg still has a test block' }
$keep = Join-Path $Out 'user_slot0_backup'
New-Item -ItemType Directory -Force $keep | Out-Null
if (-not $LoadOnly) { Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $keep -Force }

function Run($tag, $lines) {
	$dir = Join-Path $Out $tag; New-Item -ItemType Directory -Force $dir | Out-Null
	Set-Content $cfg ($orig.TrimEnd() + "`r`n# ddpr_xref run`r`n" + (($lines | ForEach-Object { "$_`r`n" }) -join '')) -NoNewline
	Start-Process 'steam://rungameid/464450'
	$p = $null; $t0 = Get-Date
	while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt 120) { Start-Sleep -Milliseconds 200; $p = Get-Process default -ErrorAction SilentlyContinue | Select-Object -First 1 }
	if (-not $p) { "$tag : game never started"; return }
	if (-not $p.WaitForExit(240000)) { "$tag : still running after 240 s, killed"; Stop-Process -Id $p.Id -Force }
	Start-Sleep 3
	# Each launch writes logs\<date>_<time>_<exe>_<pid>; copy that launch's folder whole.
	$sess = Get-ChildItem $logs -Directory -Filter "*_$($p.Id)" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
	if ($sess) { Copy-Item (Join-Path $sess.FullName '*') $dir -Recurse -Force } else { "$tag : no log folder for pid $($p.Id)" }
	"$tag : pid $($p.Id) done"
}

try {
	if ($LoadOnly) { Run 'b_load' @("D3D9SW_LOAD_AT=$LoadAt", "D3D9SW_QUIT_AT=$QuitB"); return }
	Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Remove-Item
	Run 'a_save' @("D3D9SW_SAVE_AT=$SaveAt", "D3D9SW_QUIT_AT=$QuitA")
	if (-not (Test-Path (Join-Path $game 'd3d9sw_slot0.meta'))) { throw 'launch A wrote no slot' }
	Copy-Item (Join-Path $game 'd3d9sw_slot0.json') (Join-Path $Out 'a_save') -Force
	Start-Sleep 3
	Run 'b_load' @("D3D9SW_LOAD_AT=$LoadAt", "D3D9SW_QUIT_AT=$QuitB")
} finally {
	Set-Content $cfg $orig -NoNewline
	if (-not $LoadOnly) {
		Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Remove-Item
		Get-ChildItem $keep -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $game -Force
	}
}
