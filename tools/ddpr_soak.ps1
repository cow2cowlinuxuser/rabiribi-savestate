# Repeated merges of one save, each in its own launch. The game gets $Watch
# seconds after the load, then is closed; a run that dies earlier is a crash.
param([string]$Save = 'F:\ddpr-merge-play14\session01', [string]$Out = 'F:\ddpr-soak', [int]$Runs = 5,
      [int]$LoadAt = 600, [int]$Watch = 40, [string[]]$Extra = @('D3D9SW_MERGE_CHECK=1', 'D3D9SW_LOSE_DEVICE=30'),
      [switch]$Chain)
# -Chain: each run loads the save the run before it took (D3D9SW_SAVE_AFTER_LOAD).
$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\DoDonPachi Resurrection'
$lose = Join-Path $game 'd3d9_sw_lose.txt'
$merge = Join-Path $PSScriptRoot 'ddpr_merge.ps1'
New-Item -ItemType Directory -Force $Out | Out-Null
for ($i = 1; $i -le $Runs; $i++) {
	if (Get-Process default -ErrorAction SilentlyContinue) { throw 'default.exe is running' }
	Remove-Item $lose -ErrorAction SilentlyContinue
	$dir = Join-Path $Out ('run{0:D2}' -f $i)
	$job = Start-Job { param($m, $s, $d, $l, $e) & $m -Save $s -Out $d -LoadAt $l -Extra $e } -ArgumentList $merge, $Save, $dir, $LoadAt, $Extra
	$p = $null; $t0 = Get-Date
	while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt 120) { Start-Sleep -Milliseconds 250; $p = Get-Process default -ErrorAction SilentlyContinue | Select-Object -First 1 }
	if (-not $p) { "run $i : game never started"; Stop-Job $job; continue }
	$deadline = $p.StartTime.AddSeconds($LoadAt / 60 + 5 + $Watch)
	while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
	$alive = -not $p.HasExited
	$secs = [math]::Round(((Get-Date) - $p.StartTime).TotalSeconds)
	$bin = Join-Path $game 'd3d9sw_slot0.json'
	$resaved = (Test-Path $bin) -and (Get-Item $bin).LastWriteTime -gt $p.StartTime.AddSeconds($LoadAt / 60 + 2)
	if ($resaved) { $sd = Join-Path $dir 'resave'; New-Item -ItemType Directory -Force $sd | Out-Null; Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $sd }
	if ($alive) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(10000)) { Stop-Process -Id $p.Id -Force } }
	Wait-Job $job -Timeout 60 | Out-Null; Receive-Job $job -ErrorAction SilentlyContinue | Out-Null; Remove-Job $job -Force
	$l = @(if (Test-Path $lose) { Get-Content $lose; Copy-Item $lose $dir })
	$reset = ($l | Select-String 'called Reset').Line -replace '^\d+ lose-device: ', ''
	$log = Get-ChildItem $dir -Recurse -Filter savestate.txt | Select-Object -First 1
	$fault = if ($log) { (Get-Content $log.FullName | Select-String '^fault:' | Select-Object -First 1).Line } else { 'no log' }
	if ($log -and (Select-String -Path $log.FullName -Pattern 'load-after-save:' -Quiet)) { $fault = "reloaded own save | $fault" }
	$from = $Save
	$stop = $false
	if ($Chain) { if ($resaved) { $Save = Join-Path $dir 'resave' } else { $stop = $true } }
	"run $i from $(Split-Path (Split-Path $from) -Leaf)\$(Split-Path $from -Leaf) : {0} at {1} s | reset: {2} | resave: {3} | {4}" -f $(if ($alive) { 'ALIVE' } else { 'DIED' }), $secs, $(if ($reset) { $reset } else { 'none' }), $(if ($resaved) { 'written' } else { 'none' }), $(if ($fault) { $fault } else { 'no fault' })
	if ($stop) { "no fresh save from run $i to chain from - stopping"; break }
	Start-Sleep 3
}
