# Set (or clear) IMAGE_FILE_LARGE_ADDRESS_AWARE on a 32-bit executable.
#
# A non-LAA 32-bit process tops out at 2 GB of user address space; the ceiling is
# fixed by the loader at CreateProcess time from the EXE's COFF Characteristics,
# so nothing in-process can raise it. On a 64-bit OS an LAA 32-bit exe instead
# gets the full 4 GB, which is the only honest way to make room above 0x7FFFFFFF
# for both the game's ~1.4 GB and an arena we own.
#
# This is a manual, reversible tool - it is NOT run by build.ps1 and never touches
# a file on its own. It writes a .laabak beside the target before the first patch.
#
# The bit is 0x0020 in the 16-bit COFF Characteristics field, which sits 22 bytes
# past the PE signature (e_lfanew + 4 + 18), i.e. two bytes before the optional
# header. This edits only that field; the DOS/PE headers are plaintext even on a
# SteamStub-encrypted body, so no ciphertext is touched - but SteamStub may still
# checksum the header, so treat a Steam complaint as data, restore, and move on.
#
# Usage:
#   tools\laa.ps1  <path-to-exe>            # set LAA
#   tools\laa.ps1  <path-to-exe> -Clear     # clear LAA (or just restore the .laabak)
#   tools\laa.ps1  <path-to-exe> -WhatIf    # report the current bit, change nothing

param(
	[Parameter(Mandatory = $true)][string]$Path,
	[switch]$Clear,
	[switch]$WhatIf
)

$LAA = 0x0020

# .NET file APIs resolve relative paths against the process directory, not $PWD -
# the same trap noaslr.ps1 hit. Resolve to absolute first.
if (-not (Test-Path $Path)) { Write-Host "  $Path not found"; exit 1 }
$Path = (Resolve-Path -LiteralPath $Path).Path

$b = [IO.File]::ReadAllBytes($Path)
$pe = [BitConverter]::ToUInt32($b, 0x3C)
if ([BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { Write-Host "  not a PE"; exit 1 }

$machine = [BitConverter]::ToUInt16($b, $pe + 4)
if ($machine -ne 0x14C) {
	Write-Host ("  machine is 0x{0:X4}, not x86 (0x14C) - LAA only means anything for a 32-bit image" -f $machine)
}

$chOff = $pe + 22
$ch = [BitConverter]::ToUInt16($b, $chOff)
$isSet = [bool]($ch -band $LAA)
Write-Host ("  {0}: Characteristics 0x{1:X4} at file offset 0x{2:X}, LARGE_ADDRESS_AWARE {3}" -f `
	(Split-Path $Path -Leaf), $ch, $chOff, $(if ($isSet) { "SET" } else { "clear" }))

$new = if ($Clear) { $ch -band (-bnot $LAA) } else { $ch -bor $LAA }
if ($new -eq $ch) {
	Write-Host "  already in the requested state - nothing to do"
	exit 0
}
if ($WhatIf) {
	Write-Host ("  would change 0x{0:X4} -> 0x{1:X4}" -f $ch, $new)
	exit 0
}

# One backup, taken before the first edit and never overwritten, so a restore
# always lands on the original vendor bytes rather than a previous patch.
$bak = "$Path.laabak"
if (-not (Test-Path $bak)) {
	[IO.File]::Copy($Path, $bak)
	Write-Host "  wrote backup $bak"
}

[BitConverter]::GetBytes([uint16]$new).CopyTo($b, $chOff)
[IO.File]::WriteAllBytes($Path, $b)
Write-Host ("  {0}: 0x{1:X4} -> 0x{2:X4}, LARGE_ADDRESS_AWARE {3}" -f `
	(Split-Path $Path -Leaf), $ch, $new, $(if ($Clear) { "cleared" } else { "set" }))
Write-Host "  restore with:  Copy-Item -Force `"$bak`" `"$Path`""
