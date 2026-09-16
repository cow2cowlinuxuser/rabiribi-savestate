# Clear DYNAMICBASE on our own wrapper DLLs.
#
# The game's private heap layout is now reproducible across launches, reboots
# and machines - a boot-1 trace and a boot-2 trace differ in exactly one line,
# the recorded image base. What still moves is where modules land, and a
# savestate holds pointers into them. Every module we pin removes one delta a
# cross-session restore would otherwise have to reconcile.
#
# Only the DllCharacteristics bit is touched. ImageBase is set at link time with
# -Wl,--image-base and must NOT be rewritten here: the loader relocates by
# (load address - header ImageBase), so editing that field afterwards without
# rewriting the image would leave every absolute address in the code pointing at
# where the linker originally put it. The relocation table is left intact too,
# so if a base is ever occupied the loader can still move the image rather than
# fail to load it - this is a preference, not a demand.
#
# It only works because system-wide ForceRelocateImages is off. If mandatory
# ASLR is ever turned on, Windows relocates regardless and this becomes a no-op;
# the check below says so rather than reporting a success it did not have.

param([string[]]$Path)

$force = $null
try { $force = (Get-ProcessMitigation -System -EA Stop).ASLR.ForceRelocateImages } catch { }
if ($force -and $force -ne 'NOTSET' -and $force -ne 'OFF') {
	Write-Host "  system ForceRelocateImages is $force - Windows will relocate these anyway" -ForegroundColor Yellow
}

foreach ($p in $Path) {
	if (-not (Test-Path $p)) { Write-Host "  $p missing"; continue }

	$b = [IO.File]::ReadAllBytes($p)
	$pe = [BitConverter]::ToUInt32($b, 0x3C)
	if ([BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { Write-Host "  $p is not a PE"; continue }

	$opt = $pe + 24
	$magic = [BitConverter]::ToUInt16($b, $opt)
	$dcOff = $opt + $(if ($magic -eq 0x10b) { 70 } else { 70 })
	$dc = [BitConverter]::ToUInt16($b, $dcOff)
	$base = if ($magic -eq 0x10b) { [BitConverter]::ToUInt32($b, $opt + 28) } else { [BitConverter]::ToUInt64($b, $opt + 24) }

	if (-not ($dc -band 0x40)) {
		Write-Host ("  {0,-22} already fixed at {1:X8}" -f (Split-Path $p -Leaf), $base)
		continue
	}
	$new = $dc -band (-bnot 0x40)
	[IO.File]::WriteAllBytes($p, $b[0..($dcOff - 1)] + [BitConverter]::GetBytes([uint16]$new) + $b[($dcOff + 2)..($b.Length - 1)])
	Write-Host ("  {0,-22} {1:X4} -> {2:X4}, pinned at {3:X8}" -f (Split-Path $p -Leaf), $dc, $new, $base)
}
