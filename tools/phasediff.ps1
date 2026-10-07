# Pair two phase inventories (d3d9sw_phases\*.jsonl) from different launches
# by identity, never by address, and say what moved and what has no partner.
#   powershell -ExecutionPolicy Bypass -File tools\phasediff.ps1 A.jsonl B.jsonl [-Detail]
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B, [switch]$Detail)

function Load($path) {
	$r = @{ module = @(); module64 = @(); thread = @(); heap = @(); alloc = @(); handle = @(); header = $null; check = $null }
	foreach ($line in Get-Content $path) {
		if (-not $line.Trim()) { continue }
		$o = $line | ConvertFrom-Json
		switch ($o.k) {
			'header' { $r.header = $o }
			'check' { $r.check = $o }
			default { if ($r.ContainsKey($o.k)) { $r[$o.k] += $o } }
		}
	}
	# The inventory's own working memory is not the process's.
	$r.alloc = @($r.alloc | ? { $_.owner -ne 'phase-inventory' })
	# Object names that carry the process id, a thread id or a GUID pair by the
	# value's place, not the value (Steam's "..._<pid>-IPCWrapper", CoreUI's
	# "PID(n)-TID(n) <guid>", COM's "\RPC Control\OLE<random hex>").
	$pidpat = "(?<!\d)$($r.header.pid)(?!\d)"
	foreach ($h in $r.handle) {
		$h.name = $h.name -replace $pidpat, '{pid}' -replace 'TID\(\d+\)', 'TID({tid})' `
			-replace '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}', '{guid}' `
			-replace '\\RPC Control\\OLE[0-9A-F]{16,}$', '\RPC Control\OLE{id}'
	}
	$r
}

# Identity keys of an allocation. A held one may carry several base holders:
# any one in common pairs it, since one of them can be a cursor that sat on
# the base in only one launch.
function AllocKeys($a) {
	if ($a.owner -like 'held:*') {
		# A cursor can sit on the base of a different block in another launch,
		# so a holder only pairs blocks of the same kind and size.
		$shape = "#$($a.type)#$($a.reserve)"
		$k = @("$($a.owner)$shape")
		if ($a.base_refs) { $k += @($a.base_refs | % { "held:$_$shape" }) }
		return $k | Select-Object -Unique
	}
	if ($a.owner -like 'file:*' -and $a.path) { return @("file:$($a.path)") }
	@("$($a.owner)#$($a.type)")
}

# Pair two lists: first by any shared key, in address order, one partner each.
function Pair($la, $lb, [scriptblock]$keys) {
	$used = @{}
	$pairs = @(); $onlyA = @()
	$index = @{}
	for ($j = 0; $j -lt $lb.Count; $j++) {
		foreach ($k in & $keys $lb[$j]) {
			if (-not $index.ContainsKey($k)) { $index[$k] = New-Object System.Collections.ArrayList }
			[void]$index[$k].Add($j)
		}
	}
	foreach ($x in $la) {
		$hit = -1
		foreach ($k in & $keys $x) {
			if (-not $index.ContainsKey($k)) { continue }
			foreach ($j in $index[$k]) { if (-not $used[$j]) { $hit = $j; break } }
			if ($hit -ge 0) { break }
		}
		if ($hit -ge 0) { $used[$hit] = $true; $pairs += ,@($x, $lb[$hit]) } else { $onlyA += $x }
	}
	$onlyB = @(for ($j = 0; $j -lt $lb.Count; $j++) { if (-not $used[$j]) { $lb[$j] } })
	[pscustomobject]@{ pairs = $pairs; onlyA = $onlyA; onlyB = $onlyB; byShape = 0 }
}

# Leftover allocations named differently in the two launches (a cursor made
# one "held", the other's header made it a "child") pair when their shape is
# unique among the leftovers on both sides. Counted apart: weaker evidence.
function PairByShape($res) {
	$shape = { param($a) "$($a.type)#$($a.reserve)#$($a.prot)" }
	$ga = $res.onlyA | Group-Object { & $shape $_ }
	$gb = @{}; $res.onlyB | Group-Object { & $shape $_ } | % { $gb[$_.Name] = $_.Group }
	$take = @{}
	foreach ($g in $ga) {
		if ($g.Count -eq 1 -and $gb.ContainsKey($g.Name) -and $gb[$g.Name].Count -eq 1) {
			$res.pairs += ,@($g.Group[0], $gb[$g.Name][0]); $take[$g.Name] = $true; $res.byShape++
		}
	}
	$res.onlyA = @($res.onlyA | ? { -not $take[(& $shape $_)] })
	$res.onlyB = @($res.onlyB | ? { -not $take[(& $shape $_)] })
	$res
}

# Two phases of one process: an allocation can change owner between them (our
# reserved chunk becomes a heap), but at the same base, type and size it is
# the same allocation. Counted with the shape pairs.
function PairSameProcess($res) {
	$byBase = @{}; foreach ($x in $res.onlyB) { $byBase["$($x.base)#$($x.type)#$($x.reserve)"] = $x }
	$keep = @()
	foreach ($x in $res.onlyA) {
		$k = "$($x.base)#$($x.type)#$($x.reserve)"
		if ($byBase.ContainsKey($k)) { $res.pairs += ,@($x, $byBase[$k]); $byBase.Remove($k); $res.byShape++ } else { $keep += $x }
	}
	$res.onlyA = $keep
	$res.onlyB = @($res.onlyB | ? { $byBase.ContainsKey("$($_.base)#$($_.type)#$($_.reserve)") })
	$res
}

$ia = Load $A; $ib = Load $B
"A: pid $($ia.header.pid) phase $($ia.header.phase)  B: pid $($ib.header.pid) phase $($ib.header.phase)"
if ($ia.header.exe -ne $ib.header.exe) { "WARNING: different executables" }
foreach ($s in @($ia, $ib)) {
	if ($s.check.handle_selftest -ne 'ok') { "WARNING: pid $($s.header.pid) handle self-test: $($s.check.handle_selftest)" }
}

$rows = @()
$details = @()
function Report($name, $res, [scriptblock]$same, [scriptblock]$label) {
	$moved = @($res.pairs | ? { -not (& $same $_[0] $_[1]) })
	$script:rows += [pscustomobject]@{
		what = $name; A = $res.pairs.Count + $res.onlyA.Count; B = $res.pairs.Count + $res.onlyB.Count
		paired = $res.pairs.Count; by_shape = $res.byShape; same = $res.pairs.Count - $moved.Count
		moved = $moved.Count; onlyA = $res.onlyA.Count; onlyB = $res.onlyB.Count
	}
	foreach ($p in $moved) { $script:details += "  $name moved:  $(& $label $p[0])  ->  $(& $label $p[1])" }
	foreach ($x in $res.onlyA) { $script:details += "  $name only in A:  $(& $label $x)" }
	foreach ($x in $res.onlyB) { $script:details += "  $name only in B:  $(& $label $x)" }
}

Report 'modules' (Pair $ia.module $ib.module { param($m) @($m.name.ToLower()) }) `
	{ param($x, $y) $x.base -eq $y.base -and $x.stamp -eq $y.stamp } { param($m) "$($m.name)@$($m.base)" }
Report 'modules64' (Pair $ia.module64 $ib.module64 { param($m) @($m.name.ToLower()) }) `
	{ param($x, $y) $x.base -eq $y.base } { param($m) "$($m.name)@$($m.base)" }
Report 'threads' (Pair $ia.thread $ib.thread { param($t) @($t.start) }) `
	{ param($x, $y) "$($x.stack)" -eq "$($y.stack)" } { param($t) "$($t.start) stack $($t.stack -join '-')" }
# A heap can be held by several variables, and its name is the first found:
# pair on any of them, then name B's heap as A's everywhere it appears.
$heapKeys = { param($h) @("$($h.owner)#$($h.bits)") + @($h.base_refs | ? { $_ } | % { "heap:$_#$($h.bits)" }) }
$hp = Pair $ia.heap $ib.heap $heapKeys
foreach ($bits in 32, 64) {
	$la = @($hp.onlyA | ? { $_.bits -eq $bits }); $lb = @($hp.onlyB | ? { $_.bits -eq $bits })
	if ($la.Count -eq 1 -and $lb.Count -eq 1) {
		$hp.pairs += ,@($la[0], $lb[0]); $hp.byShape++
		$hp.onlyA = @($hp.onlyA | ? { $_ -ne $la[0] }); $hp.onlyB = @($hp.onlyB | ? { $_ -ne $lb[0] })
	}
}
foreach ($p in $hp.pairs) {
	if ($p[0].owner -eq $p[1].owner) { continue }
	$from = $p[1].owner; $to = $p[0].owner
	foreach ($al in $ib.alloc) {
		$al.owner = ($al.owner -split '\|' | % { if ($_ -eq $from) { $to } else { $_ -replace "^child:$([regex]::Escape($from)):", "child:${to}:" } }) -join '|'
	}
	$p[1].owner = $to
}
Report 'heaps' $hp { param($x, $y) $x.base -eq $y.base } { param($h) "$($h.owner)@$($h.base)" }
if ($ia.header.pid -eq $ib.header.pid) {
	# Same process: an unchanged address is the strongest identity there is.
	$ar = PairSameProcess ([pscustomobject]@{ pairs = @(); onlyA = $ia.alloc; onlyB = $ib.alloc; byShape = 0 })
	$ar.byShape = 0
	$rest = Pair $ar.onlyA $ar.onlyB ${function:AllocKeys}
	$ar = [pscustomobject]@{ pairs = $ar.pairs + $rest.pairs; onlyA = $rest.onlyA; onlyB = $rest.onlyB; byShape = 0 }
} else {
	$ar = Pair $ia.alloc $ib.alloc ${function:AllocKeys}
}
Report 'allocations' (PairByShape $ar) `
	{ param($x, $y) $x.base -eq $y.base -and $x.reserve -eq $y.reserve } `
	{ param($a) "$($a.owner) $($a.type) $($a.base)+$($a.reserve)" }
Report 'handles' (Pair $ia.handle $ib.handle { param($h) @("$($h.type)|$($h.name)") }) `
	{ param($x, $y) $x.h -eq $y.h } { param($h) "$($h.type) '$($h.name)' h=$($h.h)" }

$rows | Format-Table -AutoSize | Out-String -Width 200
if ($Detail) { $details } else { "$($details.Count) detail line(s); -Detail lists them" }
