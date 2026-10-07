# Run xfuzz in pairs of launches: save in one, load in the next, on layouts
# disturbed by different seeds. Each pair appends one line to
# det\xfuzz\xfuzz_report.txt; a pair whose second launch dies before it can
# report gets the savestate log's fault line instead.
#
#   tools\xfuzz\xfuzz.ps1 [-Trials 10] [-Same] [-Build]
#
# -Same runs the control: save and load in one launch, where nothing should break.

param([int]$Trials = 10, [switch]$Same, [switch]$Build, [int]$SaveAt = 30, [int]$Timeout = 90)

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$work = Join-Path $root "det\xfuzz"
$null = New-Item -ItemType Directory -Force -Path $work
$log = Join-Path $work "d3d9_sw_savestate_xfuzz.txt"

if ($Build -or -not (Test-Path (Join-Path $work "xfuzz.exe"))) {
	zig cc -O2 -target x86-windows-gnu -shared -o (Join-Path $work "xfuzz_mod.dll") (Join-Path $PSScriptRoot "xfuzz_mod.c")
	if ($LASTEXITCODE) { exit 1 }
	zig cc -O2 -target x86-windows-gnu "-Wl,--subsystem,windows" -o (Join-Path $work "xfuzz.exe") (Join-Path $PSScriptRoot "xfuzz.c") -ld3d9 -luser32 -lgdi32 "-Wl,--image-base=0x00400000" "-Wl,--no-dynamicbase"
	if ($LASTEXITCODE) { exit 1 }
}
Copy-Item (Join-Path $root "x86\d3d9.dll") $work -Force
$cfg = Join-Path $work "d3d9_sw.cfg"
if (-not (Test-Path $cfg)) {
	"D3D9SW_SLOTFILE=1`r`nD3D9SW_EXCLFORCE=1`r`nD3D9SW_REWIND_NEWTHREADS=run`r`n" | Set-Content $cfg -NoNewline
}

Add-Type -Name XfW -Namespace XfU -MemberDefinition @'
public delegate bool CB(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(CB cb, System.IntPtr l);
[DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
'@

# csrss draws the "Application Error" box after the process is already gone,
# so it has to be answered by window rather than killed by process.
function CloseCrashBoxes {
	$cb = [XfU.XfW+CB] {
		param($h, $l)
		$sb = New-Object System.Text.StringBuilder 256
		[void][XfU.XfW]::GetWindowText($h, $sb, 256)
		if ($sb.ToString() -like "xfuzz*Application Error*") { [void][XfU.XfW]::PostMessage($h, 0x111, [IntPtr]1, [IntPtr]::Zero) }
		$true
	}
	[void][XfU.XfW]::EnumWindows($cb, [IntPtr]::Zero)
}

function Launch([string]$args_, [hashtable]$env_) {
	foreach ($k in 'D3D9SW_SAVE_AT', 'D3D9SW_LOAD_AT', 'D3D9SW_QUIT_AT', 'D3D9SW_SAVE_AFTER_LOAD') { Remove-Item "env:$k" -EA SilentlyContinue }
	foreach ($k in $env_.Keys) { Set-Item "env:$k" $env_[$k] }
	$p = Start-Process (Join-Path $work "xfuzz.exe") -ArgumentList $args_ -WorkingDirectory $work -PassThru
	$done = $p.WaitForExit($Timeout * 1000)
	if (-not $done) { Stop-Process -Id $p.Id -Force -EA SilentlyContinue }
	Get-Process WerFault -EA SilentlyContinue | ? { $_.MainWindowTitle -like "*xfuzz*" } | Stop-Process -Force -EA SilentlyContinue
	CloseCrashBoxes
	if (-not $done) { return "TIMEOUT" }
	return "exit $($p.ExitCode)"
}

$report = Join-Path $work "xfuzz_report.txt"
for ($t = 1; $t -le $Trials; $t++) {
	$a = Get-Random -Minimum 1 -Maximum 100000
	$b = Get-Random -Minimum 1 -Maximum 100000
	$shift = Get-Random -Minimum 0 -Maximum 2
	$before = if (Test-Path $report) { (Get-Content $report).Count } else { 0 }
	Remove-Item $log -Force -EA SilentlyContinue
	if ($Same) {
		$r = Launch "$a $shift" @{ D3D9SW_SAVE_AT = $SaveAt; D3D9SW_LOAD_AT = $SaveAt * 2 }
	} else {
		Get-ChildItem $work -Filter 'd3d9sw_slot*' | Remove-Item -Force
		# The restore rewinds QUIT_AT's counter to the first launch's, so the
		# second launch ends 40 frames after its load; the save comes at 5.
		$r1 = Launch "$a 0" @{ D3D9SW_SAVE_AT = $SaveAt; D3D9SW_QUIT_AT = $SaveAt + 40 }
		Remove-Item $log, (Join-Path $work "xfuzz_render.txt") -Force -EA SilentlyContinue
		$r = Launch "$b $shift" @{ D3D9SW_LOAD_AT = $SaveAt; D3D9SW_QUIT_AT = $SaveAt + 60; D3D9SW_SAVE_AFTER_LOAD = 5 }
	}
	$after = if (Test-Path $report) { (Get-Content $report).Count } else { 0 }
	$resave = ''
	if (-not $Same -and (Test-Path $log)) {
		$txt = Get-Content $log -Raw
		$i = $txt.IndexOf('save-after-load: taking')
		$resave = if ($i -lt 0) { ' | resave: never ran' }
			elseif ($txt.IndexOf("`nsave: slot", $i) -gt 0) { ' | resave: ok' }
			else { Copy-Item $log (Join-Path $work "resave_fail_$t.txt") -Force; ' | resave: DIED ' + (Select-String -Path $log -Pattern '^fault: ' | Select -Last 1).Line }
	}
	$rf = Join-Path $work "xfuzz_render.txt"
	if (-not $Same) { $resave += if (Test-Path $rf) { ' | ' + @(Get-Content $rf)[-1] } else { ' | render: never checked' } }
	if ($after -gt $before) {
		Write-Host ("[{0}] {1}{2}" -f $t, (Get-Content $report)[-1], $resave)
	} else {
		$why = if (Test-Path $log) {
			(Select-String -Path $log -Pattern '^fault: |^load: refused|refused - ' | Select -First 1).Line
		}
		$line = "NOREPORT seed $a->$b shift $shift | $r | $why"
		Add-Content $report $line
		Write-Host "[$t] $line" -ForegroundColor Yellow
	}
}
