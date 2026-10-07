# Records as much of a DDPR run as Windows will hand over.
#   Start: the game reaching D3D9SW_LOAD_AT with D3D9SW_REC_GATE=1 (it waits there
#   until TTD is attached), or 0 (top row or numpad). Under TTD our rasterizer
#   runs at well under a frame a second, so starting any earlier than the load
#   buys gigabytes of title screen.
#   0 again, or the game exiting: stop and write everything out.
# What is recorded:
#   - TTD: every user-mode instruction default.exe executes, with every memory
#     read and write, replayable backwards in WinDbg. Full mode to disk, not ring:
#     a 32-bit ring lives inside the game's own address space, which is full.
#   - ETW kernel, held in RAM until the stop: process/thread/image, context
#     switches and readying, every system call with its stack, ALPC, file, disk,
#     registry, page faults, virtual alloc/free, handle and object events, timers,
#     CPU samples.
#   - ETW user providers, also in RAM: Win32k (focus, foreground, input, message
#     routing), Kernel-Process, Kernel-Memory, Kernel-Audit-API-Calls, HID input.
# Run it before or after the game starts; it waits for default.exe. Needs admin
# and relaunches itself elevated. -Stop tears down anything a killed run left.
# -Modules limits TTD to those modules (and what they call), which leaves our
# rasterizer's worker threads unrecorded: all of them recorded is ~5 frames in
# half a minute. Empty records everything.
param([string]$Out = 'D:\ddpr-rec', [int]$NumVCpu = 8, [int]$RamMB = 6144, [switch]$NoTtd,
	[string[]]$Modules = @('default.exe', 'dat112.bin'), [switch]$Stop)
$ErrorActionPreference = 'Continue'
$Modules = @($Modules | ForEach-Object { $_ -split ',' } | Where-Object { $_ -and $_ -ne "''" })
$xperf = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
$user = 'DdprRecUser'

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
	$a = @('-NoExit', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Out', "`"$Out`"",
		'-NumVCpu', $NumVCpu, '-RamMB', $RamMB)
	if ($NoTtd) { $a += '-NoTtd' }
	if ($Stop) { $a += '-Stop' }
	$a += '-Modules'; $a += ($(if ($Modules) { $Modules -join ',' } else { "''" }))
	# Minimized, so it does not take the foreground from the game.
	Start-Process (Get-Process -Id $PID).Path -Verb RunAs -WindowStyle Minimized -ArgumentList $a
	return
}

function Stop-Etw([string]$dir) {
	if ($dir) {
		& $xperf -flush -f "$dir\kernel_raw.etl" 2>&1 | Out-Host
		& $xperf -flush $user -f "$dir\user_raw.etl" 2>&1 | Out-Host
	}
	& $xperf -stop 2>&1 | Out-Null
	& $xperf -stop $user 2>&1 | Out-Null
	if ($dir -and (Test-Path "$dir\kernel_raw.etl")) {
		$parts = @("$dir\kernel_raw.etl")
		if (Test-Path "$dir\user_raw.etl") { $parts += "$dir\user_raw.etl" }
		& $xperf -merge @parts "$dir\system.etl" 2>&1 | Out-Host
	}
}

if ($Stop) {
	& ttd.exe -stop all 2>&1 | Out-Host
	Stop-Etw $null
	'stopped'
	return
}

Add-Type -Name K -Namespace DdprRec -MemberDefinition '[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);'
function Zero { (([DdprRec.K]::GetAsyncKeyState(0x30) -band 0x8000) -ne 0) -or (([DdprRec.K]::GetAsyncKeyState(0x60) -band 0x8000) -ne 0) }

New-Item -ItemType Directory -Force $Out | Out-Null
$dir = Join-Path $Out (Get-Date -Format 'MMdd_HHmmss')
New-Item -ItemType Directory -Force $dir | Out-Null
"output: $dir"
'waiting for default.exe ...'
$game = $null
while (-not $game) { Start-Sleep -Milliseconds 200; $game = Get-Process default -ErrorAction SilentlyContinue | Select-Object -First 1 }
"default.exe pid $($game.Id) - recording starts at the game's load point (D3D9SW_REC_GATE=1), or press 0"

# The game creates these at its load point; until then OpenExisting throws.
function Open-Ev([string]$n) { try { [Threading.EventWaitHandle]::OpenExisting("Local\$n") } catch { $null } }
# Anything still holding them (an earlier recorder window left open) keeps them
# alive and set from the last run, which would start this one at once.
foreach ($n in 'DdprRecWant', 'DdprRecGo') { $e = Open-Ev $n; if ($e) { [void]$e.Reset(); $e.Dispose(); "reset a leftover $n" } }
$why = $null
while (-not $why) {
	if ($game.HasExited) { 'game exited before recording started - nothing recorded'; return }
	if (Zero) { $why = '0 pressed' }
	else {
		$want = Open-Ev 'DdprRecWant'
		if ($want -and $want.WaitOne(0)) { $why = 'the game reached its load point and is waiting' }
		Start-Sleep -Milliseconds 20
	}
}
$t0 = Get-Date
"[$($t0.ToString('HH:mm:ss.fff'))] $why - starting"
[console]::Beep(880, 120)

$kflags = 'PROC_THREAD+LOADER+PROFILE+CSWITCH+DISPATCHER+SYSCALL+ALPC+FILE_IO+FILE_IO_INIT+DISK_IO+HARD_FAULTS+REGISTRY+VIRT_ALLOC+OB_HANDLE+OB_OBJECT+TIMER+DEBUG_EVENTS'
$kstacks = 'SyscallEnter+CSwitch+ReadyThread+VirtualAlloc+VirtualFree+HandleCreate+HandleClose+FileCreate+RegOpenKey+Profile+ThreadCreate+ImageLoad'
$bufs = [math]::Max(64, [int]($RamMB * 5 / 6))
& $xperf -on $kflags -stackwalk $kstacks -BufferSize 1024 -MinBuffers 256 -MaxBuffers $bufs -Buffering 2>&1 | Out-Host
"kernel session: exit $LASTEXITCODE"
$uprov = 'Microsoft-Windows-Win32k+Microsoft-Windows-Kernel-Process+Microsoft-Windows-Kernel-Memory+Microsoft-Windows-Kernel-Audit-API-Calls+Microsoft-Windows-Input-HIDCLASS'
& $xperf -start $user -on $uprov -BufferSize 1024 -MinBuffers 64 -MaxBuffers ([math]::Max(32, [int]($RamMB / 6))) -Buffering 2>&1 | Out-Host
"user session: exit $LASTEXITCODE"

$ttd = $null
if (-not $NoTtd) {
	$ta = @('-accepteula', '-noUI', '-numVCpu', $NumVCpu, '-onInitCompleteEvent', 'Local\DdprRecGo', '-out', "`"$dir`"")
	foreach ($m in ($Modules | Where-Object { $_ })) { $ta += '-module'; $ta += $m }
	$ta += '-attach'; $ta += $game.Id
	"ttd: $($ta -join ' ')"
	$ttd = Start-Process ttd.exe -PassThru -WindowStyle Minimized -RedirectStandardOutput "$dir\ttd_out.txt" -RedirectStandardError "$dir\ttd_err.txt" -ArgumentList $ta
	"ttd attaching to $($game.Id) (recorder pid $($ttd.Id))"
}
# TTD sets Go when it is recording; if it never does, let the game go anyway.
$go = Open-Ev 'DdprRecGo'
if ($go) {
	if ($ttd -and $go.WaitOne(90000)) { "[$((Get-Date).ToString('HH:mm:ss.fff'))] ttd is recording - game released" }
	else { [void]$go.Set(); "[$((Get-Date).ToString('HH:mm:ss.fff'))] released the game without ttd's signal" }
}
"[$((Get-Date).ToString('HH:mm:ss.fff'))] RECORDING - press 0 again, or close the game, to stop"

while (Zero) { Start-Sleep -Milliseconds 20 }
while (-not $game.HasExited -and -not (Zero)) { Start-Sleep -Milliseconds 20 }
$t1 = Get-Date
"[$($t1.ToString('HH:mm:ss.fff'))] stopping after $([math]::Round(($t1 - $t0).TotalSeconds, 1)) s ($(if ($game.HasExited) { 'game exited' } else { '0 pressed' }))"
[console]::Beep(440, 120)

if ($ttd) {
	if (-not $game.HasExited) { & ttd.exe -stop $game.Id 2>&1 | Out-Host }
	if (-not $ttd.WaitForExit(600000)) { 'ttd did not finish within 10 min' }
}
Stop-Etw $dir
foreach ($e in @($want, $go)) { if ($e) { [void]$e.Reset(); $e.Dispose() } }
Get-ChildItem $dir | ForEach-Object { '{0,-28} {1,10:N0} MB' -f $_.Name, ($_.Length / 1MB) }
'done'
