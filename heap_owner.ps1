# Who actually allocated the blocks on the shared process heap.
#
# BLKOWNER currently guesses ownership from a block's contents - does it hold
# pointers into game code or into system code. That is a heuristic, and we have
# never checked it against the truth. With gflags +ust set on the image, Windows
# records the call stack for every allocation, so the truth is sitting in the
# process and only needs reading out.
#
# Attaches NON-INVASIVELY (-pv), the same way grab_code.ps1 does: cdb reads
# memory without DebugActiveProcess, so IsDebuggerPresent stays false and the
# Steam DRM stub is none the wiser. Nothing is written to the process.
#
# Sampling is by size class rather than block-by-block. A 300 MB heap has
# millions of blocks and dumping them all is useless, but the largest size
# classes account for most of the bytes, so naming who allocates those
# characterises the bulk of the heap for a bounded amount of work.

$cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"
$dir = "F:\rbo_fabre_proto\d3d9_sw"
$out = "$dir\heap_owner.txt"
$raw = "$dir\heap_owner_raw.txt"

# Symbols are deliberately not fetched. The ust stack is recorded as return
# addresses at allocation time, so no unwinding is needed to read it, and the
# only thing we want from each frame is which module it lands in - which comes
# from the module list, not from a PDB. Wiring up a symbol server would only
# make the attach slower and the game's frames would still be rabiribi+0x....
$env:_NT_SYMBOL_PATH = ""

function Invoke-Cdb($procId, [string[]]$cmds) {
    $joined = ($cmds + "q") -join "; "
    & $cdb -pv -p $procId -c $joined 2>&1 |
        Where-Object {
            $_ -notmatch 'Repository|Preparing the env|Waiting for Debug|Copyright|Microsoft \(R\)|Path validation|Deferred|ExtensionRepository|Nuget|EnableRedirect|Configuring repos|search path'
        }
}

Write-Host "waiting for rabiribi.exe..."
$p = $null
for ($i = 0; $i -lt 900; $i++) {
    $p = Get-Process rabiribi -ErrorAction SilentlyContinue
    if ($p) { break }
    Start-Sleep -Seconds 2
}
if (-not $p) { "TIMED OUT waiting for the game" | Tee-Object $out; exit 1 }
$procId = $p.Id
Write-Host "found pid $procId"
Write-Host "PLAY TO A NORMAL SPOT. Probing in 40s - the game will hitch briefly each attach."
Start-Sleep -Seconds 40

$log = New-Object Collections.ArrayList
function Note($s) { [void]$log.Add($s); Write-Host $s }

# ---- phase 1: which heaps exist, and how big -----------------------------
Note "==== phase 1: heap summary ===="
$h1 = Invoke-Cdb $procId @("!heap -s")
$h1 | Out-File $raw
$h1 | ForEach-Object { [void]$log.Add($_) }

# The handle column is the first hex field on each heap row. The game's runtime
# heap is the one our savestate log calls "the game's runtime, and the process
# heap" - in practice the largest, which is also the one worth characterising.
$heaps = @()
foreach ($line in $h1) {
    if ($line -match '^\s*([0-9a-fA-F]{6,16})\s+\d+\s+(\d+)\s+(\d+)\s') {
        $heaps += [pscustomobject]@{ Handle = $matches[1]; Committed = [int]$matches[2] }
    }
}
if (-not $heaps) {
    Note "could not parse any heap rows from !heap -s; see $raw for what it actually printed"
    $log | Set-Content $out
    exit 1
}
$game = ($heaps | Sort-Object Committed -Descending)[0].Handle
Note ""
Note "parsed $($heaps.Count) heap(s); treating $game as the game's runtime heap (largest committed)"

# ---- phase 2: which allocation sizes dominate it -------------------------
Note ""
Note "==== phase 2: size classes on heap $game ===="
$h2 = Invoke-Cdb $procId @("!heap -stat -h 0x$game -grp A 24")
$h2 | Out-File $raw -Append
$h2 | ForEach-Object { [void]$log.Add($_) }

# Rows look like:  size  #blocks  total  ( %)
$sizes = @()
foreach ($line in $h2) {
    if ($line -match '^\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+-\s+([0-9a-fA-F]+)\s+\(') {
        $sizes += $matches[1]
    }
}
$sizes = $sizes | Select-Object -First 10
Note ""
Note "top size class(es) to sample: $($sizes -join ', ')"

# ---- phase 3: pick real blocks of those sizes ----------------------------
Note ""
Note "==== phase 3: sampling blocks ===="
$fltCmds = @()
foreach ($s in $sizes) { $fltCmds += "!heap -flt s 0x$s" }
$h3 = Invoke-Cdb $procId $fltCmds
$h3 | Out-File $raw -Append

# Busy block rows carry the user pointer in the 5th hex column.
$addrs = @()
foreach ($line in $h3) {
    if ($line -match '^\s*([0-9a-fA-F]{6,8})\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\[[0-9a-fA-F]+\]\s+([0-9a-fA-F]{6,8})\s+[0-9a-fA-F]+\s+-\s+\(busy\)') {
        $addrs += $matches[2]
    }
}
$addrs = $addrs | Select-Object -Unique | Get-Random -Count ([Math]::Min(60, ($addrs | Select-Object -Unique).Count))
Note "sampled $($addrs.Count) busy block(s)"
if (-not $addrs) {
    Note "no busy blocks parsed; see $raw"
    $log | Set-Content $out
    exit 1
}

# ---- phase 4: the allocation stack for each -------------------------------
Note ""
Note "==== phase 4: allocation stacks ===="
$paCmds = @()
foreach ($a in $addrs) { $paCmds += ".echo ===BLOCK $a==="; $paCmds += "!heap -p -a 0x$a" }
$h4 = Invoke-Cdb $procId $paCmds
$h4 | Out-File $raw -Append

# Attribute each block to the first frame that is NOT the allocator itself.
# ntdll's RtlAllocateHeap and the CRT's malloc sit on top of every stack and say
# nothing about ownership; the first frame below them is the caller that wanted
# the memory, and that is the module we care about.
$noise = 'ntdll!|ntdll\+|verifier|RtlAllocate|RtlpAllocate|RtlReAllocate|RtlDebugAllocate'
$owner = @{}
$cur = $null; $claimed = $false
foreach ($line in $h4) {
    if ($line -match '^===BLOCK ([0-9a-fA-F]+)===') { $cur = $matches[1]; $claimed = $false; continue }
    if (-not $cur -or $claimed) { continue }
    if ($line -match '^\s*[0-9a-fA-F]{8}\s+[0-9a-fA-F]{8}\s+([A-Za-z0-9_\-]+)(!|\+)') {
        $mod = $matches[1]
        if ($line -match $noise) { continue }
        if ($mod -match '^(ntdll|verifier)$') { continue }
        $owner[$mod] = 1 + $(if ($owner.ContainsKey($mod)) { $owner[$mod] } else { 0 })
        $claimed = $true
    }
}

Note ""
Note "==== VERDICT: who allocated the sampled blocks ===="
if ($owner.Count -eq 0) {
    Note "no stacks resolved. Either +ust is not active on this process (was it"
    Note "launched after the gflags change?) or the frames did not parse - $raw has the raw text."
} else {
    $tot = ($owner.Values | Measure-Object -Sum).Sum
    foreach ($k in ($owner.Keys | Sort-Object { $owner[$_] } -Descending)) {
        Note ("  {0,-24} {1,4} block(s)  {2,5:N1}%" -f $k, $owner[$k], (100.0 * $owner[$k] / $tot))
    }
    Note ""
    Note "A heap dominated by rabiribi.exe means ownership is clean and BLKOWNER's job is easy."
    Note "A heap finely mixed with system modules means content-sniffing cannot separate them"
    Note "and the allocator hook is the only honest answer."
}

$log | Set-Content $out
Write-Host ""
Write-Host "wrote $out (verdict) and $raw (raw cdb output)"
