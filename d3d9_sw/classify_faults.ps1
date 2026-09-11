# Counts faults by SHAPE, not by total deaths.
#
# Exists because of a specific way the next experiment could mislead us. Moving
# ss_log off the C runtime is expected to reduce deaths; if it does, the tempting
# conclusion is that the crash is understood. But the three faults carrying the
# only positive evidence we have - a 64-bit pointer with its top half current and
# its bottom half a sentinel - could be a small minority of those deaths and
# survive a large drop completely intact. A single total hides that. Counting by
# shape cannot.
#
# Committed to BEFORE the run rather than chosen after it, so the buckets cannot
# be drawn around whatever the answer turns out to be.

param(
    [string]$Log = "",   # empty = newest d3d9_sw_savestate*.txt; the log is now per-host-exe
    [int]$Sessions = 0   # 0 = whole log; N = only the last N sessions
)

if (-not $Log) {
    $cand = Get-ChildItem -File -Filter "d3d9_sw_savestate*.txt" | Sort-Object LastWriteTime -Descending
    if (-not $cand) { Write-Error "no d3d9_sw_savestate*.txt found"; exit 1 }
    $Log = $cand[0].Name
    if ($cand.Count -gt 1) {
        "logs present: " + (($cand | ForEach-Object { $_.Name }) -join ", ")
        "using: $Log   (pass -Log to pick another)"
        ""
    }
}
if (-not (Test-Path $Log)) { Write-Error "no log at $Log"; exit 1 }
$lines = Get-Content $Log

if ($Sessions -gt 0) {
    $starts = @()
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^===== session') { $starts += $i } }
    if ($starts.Count -gt $Sessions) { $lines = $lines[$starts[-$Sessions]..($lines.Count - 1)] }
}

# A half-pointer: 64 bits where the high half is a plausible current address and
# the low half is entirely 0 or entirely 1. Two failed halves of the same value.
function Test-HalfPointer([string]$hex) {
    if ($hex -notmatch '^[0-9A-Fa-f]{16}$') { return $false }
    $hi = $hex.Substring(0, 8)
    $lo = $hex.Substring(8, 8)
    if ($hi -eq '00000000') { return $false }          # an ordinary 32-bit value
    return ($lo -eq '00000000' -or $lo -eq 'FFFFFFFF')
}

$truncated = 0
$faults = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -notmatch '^fault: ([0-9A-F]{8}) at ([0-9A-F]{16}) in (\S+)') { continue }
    # Records from before the ss_num fix printed every address cut to 32 bits (a
    # pc of 00007FFD4551C1F7 logged as 000000004551C1F7). Their module+offset was
    # still right, but their ADDRESSES are fiction, so shape-classifying them
    # would be measuring the old logging bug. Counted and set aside rather than
    # silently mixed in - a log that spans a change in how it was collected is
    # two datasets, and this script exists to stop exactly that kind of blend.
    if ($Matches[2].Substring(0, 8) -eq '00000000') { $truncated++; continue }
    $f = [pscustomobject]@{
        code   = $Matches[1]
        pc     = $Matches[2]
        where  = $Matches[3]
        addr   = ''
        access = ''
        shape  = ''
        unwound = 0
        stopped = ''
    }
    # the detail lines belonging to this fault, up to the next fault or session
    for ($j = $i + 1; $j -lt [Math]::Min($lines.Count, $i + 40); $j++) {
        $l = $lines[$j]
        if ($l -match '^fault: ' -or $l -match '^===== session') { break }
        if ($l -match '^\s+(reading|writing) ([0-9A-F]{16})') { $f.access = $Matches[1]; $f.addr = $Matches[2] }
        if ($l -match '^\s+frame \d+') { $f.unwound++ }
        if ($l -match 'NOT EXECUTABLE CODE') { $f.stopped = 'pc-not-code' }
        elseif ($l -match 'no unwind data') { $f.stopped = 'no-unwind-data' }
        elseif ($l -match 'stopped making progress') { $f.stopped = 'no-progress' }
    }

    # Ordered most specific first: a fault can match several descriptions and the
    # narrowest is the informative one.
    if (Test-HalfPointer $f.pc) { $f.shape = 'A half-pointer as the PC (control transferred to one)' }
    elseif (Test-HalfPointer $f.addr) { $f.shape = 'A half-pointer as the data address' }
    elseif ($f.addr -match '^0{12}' ) { $f.shape = 'Null or small offset (a null base + field)' }
    elseif ($f.addr -eq 'FFFFFFFFFFFFFFFF') { $f.shape = 'All-ones address (-1 used as a pointer)' }
    elseif ($f.where -match 'ucrtbase') { $f.shape = 'Inside the C runtime' }
    elseif ($f.where -match 'ntdll') { $f.shape = 'Inside ntdll' }
    elseif ($f.where -match 'mono') { $f.shape = 'Inside mono' }
    else { $f.shape = 'Other' }
    $faults += $f
}

"faults found: $($faults.Count)"
if ($truncated) {
    "  plus $truncated fault(s) SET ASIDE: logged before the address-truncation fix, so"
    "  their addresses are 32-bit fiction and cannot be classified by shape."
    "  Their module+offset is still trustworthy - use -Truncated to list them."
}
""
"by shape:"
$faults | Group-Object shape | Sort-Object Count -Descending |
    ForEach-Object { "  {0,4}  {1}" -f $_.Count, $_.Name }
""
"by exception code:"
$faults | Group-Object code | Sort-Object Count -Descending |
    ForEach-Object { "  {0,4}  {1}" -f $_.Count, $_.Name }
""
# The point of this one: it says how much of the evidence is trustworthy at all.
"unwind quality (frames per fault, and why the walk stopped):"
$faults | Group-Object stopped | Sort-Object Count -Descending |
    ForEach-Object { "  {0,4}  stopped: {1}" -f $_.Count, $(if ($_.Name) { $_.Name } else { 'reached the bottom or truncated' }) }
$deep = ($faults | Where-Object { $_.unwound -ge 3 }).Count
"  $deep of $($faults.Count) fault(s) produced 3 or more real frames"
""
"the half-pointer family in full:"
$hp = $faults | Where-Object { $_.shape -like '*half-pointer*' }
if (-not $hp) { "  none in this range" }
else { $hp | ForEach-Object { "  {0}  pc={1}  {2} {3}  in {4}" -f $_.code, $_.pc, $_.access, $_.addr, $_.where } }
