# Ground truth for who owns each block on the shared process heap.
#
# With gflags +ust on the image, ntdll records the allocating call stack for
# every heap block, and "!heap -k" prints it per entry. That turns the ownership
# question from a guess about a block's contents into a fact about who asked for
# it - which is what BLKOWNER needs and has never had.
#
# Attribution rule: walk down the recorded stack and take the first frame that
# is not the allocator itself. ntdll's Rtl*Heap and the CRT's malloc/operator
# new sit on top of every stack and say nothing about ownership; the frame below
# them is the code that actually wanted the memory.

$cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"
$dir = "F:\rbo_fabre_proto\d3d9_sw"
$raw = "$dir\heap_owner2_raw.txt"
$out = "$dir\heap_owner2.txt"
$env:_NT_SYMBOL_PATH = "srv*C:\symbols*https://msdl.microsoft.com/download/symbols"

$p = Get-Process rabiribi -ErrorAction SilentlyContinue
if (-not $p) { "game not running"; exit 1 }

# -hl to include the LFH entries, which is where the small game objects live;
# without it we would only see the handful of large non-LFH blocks.
$cmds = "!heap -k -hl 007f0000; .echo ===HEAP2===; !heap -k -hl 0fea0000; q"
& $cdb -pv -p $p.Id -c $cmds 2>&1 |
    Where-Object { $_ -notmatch 'Repository|Preparing|Waiting for Debug|Copyright|Microsoft \(R\)|Path validation|Extension|Nuget|EnableRedirect|Configuring|NatVis' } |
    Set-Content $raw

$lines = Get-Content $raw
$log = New-Object Collections.ArrayList
function Note($s) { [void]$log.Add($s); Write-Host $s }

# Frames that are allocator plumbing rather than a requester.
$plumbing = '^(ntdll|ucrtbase|msvcrt|msvcr\d+|KERNELBASE|KERNEL32|verifier)$'
$allocish = 'RtlAllocateHeap|RtlpAllocate|RtlReAllocate|RtlDebugAllocate|RtlpLowFragHeap|RtlSizeHeap|HeapAlloc|malloc|calloc|realloc|operator_new|_nh_malloc'

$byMod = @{}      # module -> [count, bytes]
$cur = $null      # pending entry: size in bytes
$want = $false

foreach ($line in $lines) {
    # Entry rows. Two shapes: the VirtualAlloc list / non-LFH form with a
    # bracketed state, and the LFH form. Both end in "busy (<requested>)".
    if ($line -match 'busy\s*\(([0-9a-fA-F]+)\)') {
        $cur = [Convert]::ToInt64($matches[1], 16)
        $want = $true
        continue
    }
    if (-not $want) { continue }

    # Stack frames are tab-indented "addr: module!symbol+off".
    if ($line -match '^\s+[0-9a-fA-F]{8}:\s+([A-Za-z0-9_\-\.]+)!(\S+)') {
        $mod = $matches[1]; $sym = $matches[2]
        if ($mod -match $plumbing -and $sym -match $allocish) { continue }
        if ($mod -match '^ntdll$' -and $sym -match 'Ldr|RtlUserThread') { continue }
        if (-not $byMod.ContainsKey($mod)) { $byMod[$mod] = @(0, [int64]0) }
        $byMod[$mod][0] += 1
        $byMod[$mod][1] += $cur
        $want = $false
        continue
    }
    # A blank line or a new section ends the stack without a verdict.
    if ($line -match '^\s*$') { $want = $false }
}

Note "==== block ownership on the process heap, from the recorded allocation stacks ===="
Note ""
if ($byMod.Count -eq 0) {
    Note "nothing attributed - see $raw"
} else {
    $tc = ($byMod.Values | ForEach-Object { $_[0] } | Measure-Object -Sum).Sum
    $tb = ($byMod.Values | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum
    Note ("  {0,-26} {1,8}  {2,7}   {3,7}   {4,7}" -f "allocated by", "blocks", "blk %", "bytes", "byte %")
    Note ("  " + ("-" * 68))
    foreach ($k in ($byMod.Keys | Sort-Object { $byMod[$_][1] } -Descending)) {
        $c = $byMod[$k][0]; $b = $byMod[$k][1]
        Note ("  {0,-26} {1,8}  {2,6:N1}%  {3,7:N0}K  {4,6:N1}%" -f `
              $k, $c, (100.0 * $c / $tc), ($b / 1024.0), (100.0 * $b / $tb))
    }
    Note ("  " + ("-" * 68))
    Note ("  {0,-26} {1,8}          {2,7:N0}K" -f "total", $tc, ($tb / 1024.0))

    $game = 0
    foreach ($k in $byMod.Keys) { if ($k -match '^rabiribi$') { $game += $byMod[$k][1] } }
    Note ""
    Note ("rabiribi.exe accounts for {0:N1}% of the attributed bytes." -f (100.0 * $game / $tb))
    Note "High means ownership is clean and BLKOWNER's content sniffing has an easy job."
    Note "Low or mixed means the game's state is interleaved with Steam and CRT blocks,"
    Note "and only a real allocator hook can separate them."
}
$log | Set-Content $out
Write-Host ""
Write-Host "wrote $out and $raw"
