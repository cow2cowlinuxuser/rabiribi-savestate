param([Parameter(Mandatory)][string]$Ref, [Parameter(Mandatory)][string[]]$Other)
# Pairs live blocks of two gh_ledger.txt files by identity (role name, site,
# size, ordinal) and reports how many pair, how many sit at the same address,
# and where the first one that moved arrived.

function Read-Ledger($path) {
	$roles = @{}; $blocks = @{}
	foreach ($l in Get-Content $path) {
		if ($l -match '^# role (R\d+) (\S+) ') { $roles[$Matches[1]] = $Matches[2]; continue }
		if ($l -match '^#') { continue }
		$x = $l -split ' '
		$key = '{0}|{1}|{2}|{3}' -f $roles[$x[4]], $x[2], $x[1], $x[3]
		$blocks[$key] = [pscustomobject]@{ Addr = $x[0]; Role = $roles[$x[4]]; Arrival = [int]$x[5]; RoleOp = [int]$x[6]; Frame = [int]$x[7]; Ms = [int]$x[8] }
	}
	$blocks
}

$a = Read-Ledger $Ref
foreach ($o in $Other) {
	$b = Read-Ledger $o
	$pair = 0; $same = 0; $byRole = @{}; $moved = @()
	foreach ($k in $a.Keys) {
		$role = $a[$k].Role
		if (-not $byRole[$role]) { $byRole[$role] = @(0, 0, 0) }
		$byRole[$role][0]++
		if (-not $b.ContainsKey($k)) { continue }
		$pair++; $byRole[$role][1]++
		if ($a[$k].Addr -eq $b[$k].Addr) { $same++; $byRole[$role][2]++ } else { $moved += [pscustomobject]@{ Key = $k; A = $a[$k]; B = $b[$k] } }
	}
	'{0}: {1} live here, {2} in ref; {3} pair by identity, {4} at the same address ({5:P1} of paired)' -f (Split-Path (Split-Path $o) -Leaf), $b.Count, $a.Count, $pair, $same, ($same / [Math]::Max(1, $pair))
	foreach ($r in $byRole.Keys | Sort-Object { -$byRole[$_][0] }) {
		$v = $byRole[$r]
		if ($v[0] -ge 5 -or $v[2] -ne $v[1]) { '    {0,-28} {1,5} live, {2,5} paired, {3,5} same address' -f $r, $v[0], $v[1], $v[2] }
	}
	$first = $moved | Sort-Object { $_.A.Arrival } | Select-Object -First 3
	foreach ($m in $first) { '    first moved: {0}  ref {1} arrival {2} frame {3}  /  here {4} arrival {5} frame {6}' -f $m.Key, $m.A.Addr, $m.A.Arrival, $m.A.Frame, $m.B.Addr, $m.B.Arrival, $m.B.Frame }
	$shift = ($moved | Where-Object { $_.A.Arrival -ne $_.B.Arrival }).Count
	'    moved blocks whose arrival number also differs: {0} of {1}' -f $shift, $moved.Count
}
