# slotpeek DIR ADDR [COUNT] - dwords from a slot file at a saved address.
param([string]$Dir, [string]$Addr, [int]$Count = 8)
$a = [Convert]::ToUInt32($Addr, 16)
$regs = Get-Content (Join-Path $Dir 'd3d9sw_slot0.json') | Where-Object { $_ -like '*"k":"region"*' } | ForEach-Object { $_ | ConvertFrom-Json }
$r = $regs | Where-Object { $b = [Convert]::ToUInt32($_.base, 16); $a -ge $b -and $a -lt $b + [Convert]::ToUInt32($_.size, 16) } | Select-Object -First 1
if (-not $r) { "{0:X8}: not in the save" -f $a; return }
$fs = [IO.File]::OpenRead((Join-Path $Dir 'd3d9sw_slot0.bin'))
$fs.Seek([Convert]::ToInt64($r.off, 16) + ($a - [Convert]::ToUInt32($r.base, 16)), 'Begin') | Out-Null
$buf = New-Object byte[] (4 * $Count); $fs.Read($buf, 0, $buf.Length) | Out-Null; $fs.Close()
"{0:X8} ({1} region at {2}): {3}" -f $a, $r.kind, $r.base, ((0..($Count - 1) | ForEach-Object { '{0:X8}' -f [BitConverter]::ToUInt32($buf, 4 * $_) }) -join ' ')
