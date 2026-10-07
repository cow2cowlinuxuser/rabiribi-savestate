# slotrefs DIR ADDR [SPAN] - saved addresses whose dword points into [ADDR, ADDR+SPAN).
param([string]$Dir, [string]$Addr, [string]$Span = '20')
if (-not ('SlotRefs' -as [type])) {
	Add-Type @'
using System; using System.IO; using System.Collections.Generic;
public static class SlotRefs {
	public static List<string> Find(string bin, long[] off, uint[] bas, uint[] size, uint lo, uint hi) {
		var hits = new List<string>(); var buf = new byte[1 << 20];
		using (var f = File.OpenRead(bin)) {
			for (int r = 0; r < off.Length; r++) {
				f.Seek(off[r], SeekOrigin.Begin); uint left = size[r], at = bas[r];
				while (left > 0) {
					int n = f.Read(buf, 0, (int)Math.Min(left, (uint)buf.Length)); if (n <= 0) break;
					for (int i = 0; i + 4 <= n; i += 4) { uint v = BitConverter.ToUInt32(buf, i); if (v >= lo && v < hi) hits.Add(string.Format("{0:X8} -> {1:X8}", at + (uint)i, v)); }
					left -= (uint)n; at += (uint)n;
				}
			}
		}
		return hits;
	}
}
'@
}
$regs = @(Get-Content (Join-Path $Dir 'd3d9sw_slot0.json') | Where-Object { $_ -like '*"k":"region"*' } | ForEach-Object { $_ | ConvertFrom-Json })
$lo = [Convert]::ToUInt32($Addr, 16); $hi = $lo + [Convert]::ToUInt32($Span, 16)
[SlotRefs]::Find((Join-Path $Dir 'd3d9sw_slot0.bin'), [long[]]($regs | ForEach-Object { [Convert]::ToInt64($_.off, 16) }),
	[uint32[]]($regs | ForEach-Object { [Convert]::ToUInt32($_.base, 16) }), [uint32[]]($regs | ForEach-Object { [Convert]::ToUInt32($_.size, 16) }), $lo, $hi)
