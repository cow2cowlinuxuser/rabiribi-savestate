param([string]$Path, [string]$Match = "")
$b = [IO.File]::ReadAllBytes($Path)
$pe = [BitConverter]::ToInt32($b, 0x3c)
$nsec = [BitConverter]::ToUInt16($b, $pe + 6)
$opt = $pe + 24
$magic = [BitConverter]::ToUInt16($b, $opt)
$dd = if ($magic -eq 0x20b) { $opt + 112 } else { $opt + 96 }
$irva = [BitConverter]::ToUInt32($b, $dd + 8)
$sec = $opt + [BitConverter]::ToUInt16($b, $pe + 20)
function Off([uint32]$rva) {
	for ($i = 0; $i -lt $nsec; $i++) {
		$s = $sec + 40 * $i
		$va = [BitConverter]::ToUInt32($b, $s + 12); $sz = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
		if ($rva -ge $va -and $rva -lt $va + $sz) { return $rva - $va + [BitConverter]::ToUInt32($b, $s + 20) }
	}
	return -1
}
function Str([int]$o) { $e = $o; while ($b[$e]) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $o, $e - $o) }
$d = Off $irva
while ($d -ge 0 -and [BitConverter]::ToUInt32($b, $d + 12)) {
	$name = Str (Off ([BitConverter]::ToUInt32($b, $d + 12)))
	$thunk = [BitConverter]::ToUInt32($b, $d); if (!$thunk) { $thunk = [BitConverter]::ToUInt32($b, $d + 16) }
	$t = Off $thunk; $fns = @()
	while ($t -ge 0 -and ($v = [BitConverter]::ToUInt32($b, $t))) {
		if ($v -band 0x80000000) { $fns += "#" + ($v -band 0xffff) } else { $fns += Str ((Off $v) + 2) }
		$t += 4
	}
	$sel = if ($Match) { $fns | ? { $_ -match $Match } } else { $fns }
	"{0} ({1}): {2}" -f $name, $fns.Count, ($sel -join " ")
	$d += 20
}
