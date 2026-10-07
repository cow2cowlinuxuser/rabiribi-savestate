param([int]$Runs = 10, [string]$SaveAt = '300', [int]$QuitAt = 420, [string]$Out = 'F:\ddpr-baseline', [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\DoDonPachi Resurrection'
$cfg = Join-Path $game 'd3d9_sw.cfg'
$log = Join-Path $game 'd3d9_sw_savestate_default.txt'
New-Item -ItemType Directory -Force $Out | Out-Null

if (Get-Process default -ErrorAction SilentlyContinue) { throw 'default.exe is running' }
$orig = Get-Content $cfg -Raw
if ($orig -match '# ddpr_baseline run') { throw 'd3d9_sw.cfg still has a ddpr_baseline block - another run is going or one died before restoring it' }

$keep = Join-Path $Out 'user_slot0_backup'
if (-not (Test-Path $keep)) {
	New-Item -ItemType Directory -Force $keep | Out-Null
	Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $keep
}
Copy-Item $cfg (Join-Path $Out 'd3d9_sw.cfg.user') -Force

try {
	Set-Content $cfg ($orig.TrimEnd() + "`r`n# ddpr_baseline run`r`nD3D9SW_SAVE_AT=$SaveAt`r`nD3D9SW_QUIT_AT=$QuitAt`r`n" + (($Extra | ForEach-Object { "$_`r`n" }) -join '')) -NoNewline
	for ($i = 1; $i -le $Runs; $i++) {
		$dir = Join-Path $Out ('run{0:D2}' -f $i)
		New-Item -ItemType Directory -Force $dir | Out-Null
		$logAt = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
		Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Remove-Item
		Start-Process 'steam://rungameid/464450'
		$p = $null; $t0 = Get-Date
		while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt 120) { Start-Sleep -Milliseconds 200; $p = Get-Process default -ErrorAction SilentlyContinue | Select-Object -First 1 }
		if (-not $p) { "run $i : game never started"; continue }
		$start = $p.StartTime; $id = $p.Id
		# With several save frames, each earlier save is copied to saveN before the next overwrites it.
		$cuts = @($SaveAt -split ',').Count; $n = 0; $last = $null; $bin = Join-Path $game 'd3d9sw_slot0.bin'
		while ($n -lt $cuts - 1 -and -not $p.HasExited) {
			Start-Sleep -Milliseconds 250
			if (-not (Test-Path $bin)) { continue }
			$w = (Get-Item $bin).LastWriteTime
			if ($w -eq $last -or $w -lt $start) { continue }
			Start-Sleep -Milliseconds 1500
			if ((Get-Item $bin).LastWriteTime -ne $w) { continue }
			$n++; $last = $w
			$sd = Join-Path $dir "save$n"; New-Item -ItemType Directory -Force $sd | Out-Null
			Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $sd
			Get-ChildItem $game -Filter 'gh_*.txt' | Copy-Item -Destination $sd
		}
		if (-not $p.WaitForExit(180000)) { "run $i : still running after 180 s, killed"; Stop-Process -Id $id -Force; Start-Sleep 2 }
		Start-Sleep 2
		$bin = Join-Path $game 'd3d9sw_slot0.bin'
		$saved = if (Test-Path $bin) { (Get-Item $bin).LastWriteTime } else { $null }
		Get-ChildItem $game -Filter 'd3d9sw_slot0.*' | Move-Item -Destination $dir
		Get-ChildItem $game -Filter 'gh_*.txt' | Move-Item -Destination $dir
		Get-ChildItem (Join-Path $game 'd3d9sw_phases') -Filter "$($id)_*" -ErrorAction SilentlyContinue | Copy-Item -Destination $dir
		if (Test-Path $log) {
			$fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $fs.Seek($logAt, 'Begin') | Out-Null
			$sr = New-Object IO.StreamReader($fs); Set-Content (Join-Path $dir 'savestate.txt') $sr.ReadToEnd(); $sr.Close()
		}
		$secs = if ($saved) { [math]::Round(($saved - $start).TotalSeconds, 1) } else { 'none' }
		"run $i : pid $id, slot written $secs s after process start"
		Start-Sleep 3
	}
} finally {
	Set-Content $cfg $orig -NoNewline
	Get-ChildItem $keep -Filter 'd3d9sw_slot0.*' | Copy-Item -Destination $game -Force
}
