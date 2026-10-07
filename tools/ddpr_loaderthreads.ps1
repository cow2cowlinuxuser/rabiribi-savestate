# Sets (or with -Remove clears) MaxLoaderThreads=1 for one game exe, so Windows
# loads its DLLs on the main thread instead of starting a pool of loader workers
# that idle out at unpredictable times. Per machine, once; asks for admin.
#
# The setting is scoped to the exe's full path (UseFilter + FilterFullPath), so
# another program with the same file name is not affected.
param(
	[string]$Exe = 'C:\Program Files (x86)\Steam\steamapps\common\DoDonPachi Resurrection\default.exe',
	[switch]$Remove
)
$Exe = [IO.Path]::GetFullPath($Exe)
$root = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\' + [IO.Path]::GetFileName($Exe)
$sub = Join-Path $root 'savestate'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
	$a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Exe', "`"$Exe`"")
	if ($Remove) { $a += '-Remove' }
	Start-Process powershell -Verb RunAs -Wait -ArgumentList $a
} else {
	# An unscoped value from an earlier version of this script.
	Remove-ItemProperty $root -Name MaxLoaderThreads -ErrorAction SilentlyContinue
	if ($Remove) {
		Remove-Item $sub -Recurse -ErrorAction SilentlyContinue
	} else {
		if (-not (Test-Path $root)) { New-Item $root -Force | Out-Null }
		New-ItemProperty $root -Name UseFilter -PropertyType DWord -Value 1 -Force | Out-Null
		New-Item $sub -Force | Out-Null
		New-ItemProperty $sub -Name FilterFullPath -PropertyType String -Value $Exe -Force | Out-Null
		New-ItemProperty $sub -Name MaxLoaderThreads -PropertyType DWord -Value 1 -Force | Out-Null
	}
	exit
}

$v = (Get-ItemProperty $sub -ErrorAction SilentlyContinue).MaxLoaderThreads
if ($v) { "MaxLoaderThreads is $v for $Exe" } else { "MaxLoaderThreads is not set for $Exe (Windows default)" }
