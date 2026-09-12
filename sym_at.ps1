# Resolve "module+RVA" from a fault line to a function, file and line.
#
# The savestate log reports our own faults as d3d11.dll+1AEF7 because that is
# all it can know from inside the process. Turning that back into a name needed
# a live process to attach to, which meant the answer was only available while
# the game was still up - and it usually is not, because the fault is why it
# went down. dbghelp will load a module straight from disk against its PDB with
# no process at all, so the log stays readable hours later.
#
#   .\sym_at.ps1 x86\d3d11.dll 1AEF7 1ACE0
param(
	[Parameter(Mandatory = $true)][string]$Image,
	[Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Rvas
)

$ErrorActionPreference = 'Stop'
$img = (Resolve-Path $Image).Path
$dir = Split-Path $img -Parent

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class Sym {
	[DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
	public static extern bool SymInitialize(IntPtr h, string path, bool invade);

	[DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
	public static extern ulong SymLoadModuleEx(IntPtr h, IntPtr file, string img,
		string mod, ulong baseAddr, uint size, IntPtr data, uint flags);

	[DllImport("dbghelp.dll", SetLastError = true)]
	public static extern uint SymSetOptions(uint opts);

	[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
	public struct SYMBOL_INFO {
		public uint SizeOfStruct, TypeIndex;
		public ulong Reserved0, Reserved1;
		public uint Index, Size;
		public ulong ModBase;
		public uint Flags;
		public ulong Value, Address;
		public uint Register, Scope, Tag, NameLen, MaxNameLen;
		[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 1024)] public string Name;
	}

	[DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
	public static extern bool SymFromAddr(IntPtr h, ulong addr, out ulong disp,
		ref SYMBOL_INFO si);

	[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
	public struct LINE64 {
		public uint SizeOfStruct;
		public IntPtr Key;
		public uint LineNumber;
		public IntPtr FileName;
		public ulong Address;
	}

	[DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
	public static extern bool SymGetLineFromAddr64(IntPtr h, ulong addr, out uint disp,
		ref LINE64 line);
}
'@

$h = [IntPtr]777
[void][Sym]::SymSetOptions(0x10 -bor 0x800000)   # LOAD_LINES | AUTO_PUBLICS
if (-not [Sym]::SymInitialize($h, $dir, $false)) { throw "SymInitialize failed" }

$base = [Sym]::SymLoadModuleEx($h, [IntPtr]::Zero, $img, $null, 0x10000000, 0, [IntPtr]::Zero, 0)
if ($base -eq 0) { throw "SymLoadModuleEx failed ($([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message))" }

Write-Host "$([IO.Path]::GetFileName($img)) loaded at 0x$('{0:X}' -f $base)`n"

foreach ($r in $Rvas) {
	$rva = [Convert]::ToUInt64(($r -replace '^0x', ''), 16)
	$addr = $base + $rva

	$si = New-Object Sym+SYMBOL_INFO
	$si.SizeOfStruct = 88
	$si.MaxNameLen = 1024
	$disp = [uint64]0

	if ([Sym]::SymFromAddr($h, $addr, [ref]$disp, [ref]$si)) {
		Write-Host ("+{0,-8} {1}+0x{2:X}" -f $r.ToUpper(), $si.Name, $disp)
	} else {
		Write-Host ("+{0,-8} <no symbol>" -f $r.ToUpper())
	}

	$ln = New-Object Sym+LINE64
	$ln.SizeOfStruct = [Runtime.InteropServices.Marshal]::SizeOf($ln)
	$ldisp = [uint32]0
	if ([Sym]::SymGetLineFromAddr64($h, $addr, [ref]$ldisp, [ref]$ln)) {
		$f = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($ln.FileName)
		Write-Host ("          {0}:{1}" -f $f, $ln.LineNumber)
	}
}
