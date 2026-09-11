# Parse savestate fault logs from THIS batch's chunk_*.txt files only.
# Emits crash_faults.csv (one row per fault) and prints aggregate histograms.
#
# Rules baked in from prior mistakes:
#   - bucket by module+offset, never by raw address (ASLR)
#   - ignore "stack-scan GUESS" lines for call-chain analysis
#   - set aside records whose faulting PC has high 32 bits zero (truncated logging)
#   - do not read prebatch / ss_final / other hosts' logs

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Csv-Escape([string]$s) {
    if ($null -eq $s) { return '' }
    if ($s -match '[",\r\n]') { return '"' + ($s -replace '"', '""') + '"' }
    return $s
}

function Hex16-ToUInt64([string]$hex) {
    if (-not $hex) { return [uint64]0 }
    $hex = $hex.Trim()
    if ($hex.Length -gt 16) { $hex = $hex.Substring($hex.Length - 16) }
    if ($hex.Length -lt 16) { $hex = $hex.PadLeft(16, '0') }
    return [Convert]::ToUInt64($hex, 16)
}

function Get-AddrShape {
    param(
        [string]$hex,
        [uint64]$rsp,
        [uint64[]]$moduleBases,
        [uint64]$stackBase,
        [uint64]$stackLimit,
        [uint64]$stackReserve
    )
    if (-not $hex -or $hex -notmatch '^[0-9A-Fa-f]{1,16}$') { return 'unknown' }
    $hex = $hex.ToUpperInvariant().PadLeft(16, '0')
    $v = Hex16-ToUInt64 $hex
    $hi = $hex.Substring(0, 8)
    $lo = $hex.Substring(8, 8)

    if ($hex -eq 'FFFFFFFFFFFFFFFF') { return 'all-ones' }
    if ($v -lt [uint64]0x10000) { return 'null-or-small' }

    $isHalf = ($hi -ne '00000000') -and ($lo -eq '00000000' -or $lo -eq 'FFFFFFFF')
    if ($isHalf) {
        $kind = if ($lo -eq '00000000') { 'half-ptr-low-zero' } else { 'half-ptr-low-ones' }
        return $kind
    }

    if ($stackBase -gt 0 -and $v -ge $stackReserve -and $v -lt $stackBase) { return 'stack' }
    if ($stackBase -gt 0 -and $v -ge $stackLimit -and $v -lt $stackBase) { return 'stack' }

    # Typical Win10+ x64 user-mode image VA.
    if ($v -ge [uint64]0x7FF000000000 -and $v -le [uint64]0x7FFFFFFFFFFF) { return 'module-like' }
    # Typical user heap / TEB-adjacent private VA (below images, above 32-bit).
    if ($v -ge [uint64]0x10000000000 -and $v -lt [uint64]0x7FF000000000) { return 'heap-like' }
    return 'other'
}

function Get-HalfPtrMatch {
    param(
        [string]$hex,
        [uint64]$rsp,
        [uint64[]]$moduleBases,
        [string]$stackNote
    )
    $notes = @()
    if (-not $hex -or $hex.Length -lt 16) { return '' }
    $hex = $hex.ToUpperInvariant().PadLeft(16, '0')
    $lo = $hex.Substring(8, 8)
    if ($lo -ne '00000000' -and $lo -ne 'FFFFFFFF') { return '' }
    $hi = $hex.Substring(0, 8)
    $hiVal = [Convert]::ToUInt32($hi, 16)
    if ($rsp -ne 0) {
        $rspHi = [uint32]($rsp -shr 32)
        if ($rspHi -eq $hiVal) { $notes += 'surviving-half-matches-RSP-high' }
    }
    foreach ($b in $moduleBases) {
        if ($b -eq 0) { continue }
        $mhi = [uint32]($b -shr 32)
        if ($mhi -eq $hiVal) { $notes += 'surviving-half-matches-a-module-base-high'; break }
    }
    return ($notes -join ';')
}

function Parse-RegRest([string]$rest) {
    $commit = ''
    $heap = ''
    $heapRewind = ''
    $side = ''
    if ($rest -match '^(committed|reserved only)') { $commit = $Matches[1] }
    if ($rest -match 'in the (.+?) heap at ([0-9A-Fa-f]+), which we (rewind|leave in the present)') {
        $heap = ($Matches[1] -replace '^the ', '').Trim()
        $heapRewind = $Matches[3]
    }
    if ($rest -match 'restored from the save') { $side = 'restored-from-save' }
    elseif ($rest -match 'held in the present') { $side = 'held-in-present' }
    elseif ($rest -match 'in neither the save nor the held set') { $side = 'neither' }
    return @{ commit = $commit; heap = $heap; heapRewind = $heapRewind; side = $side }
}

function Classify-AccessWho([string]$who) {
    if (-not $who) { return 'none' }
    $w = $who.ToLowerInvariant()
    if ($w -match 'unmapped') { return 'unmapped' }
    if ($w -match 'no-access|no access|not committed|guard') { return 'no-access' }
    return 'other'
}

# --- load chunks, in order, this batch only ---
$chunks = @(Get-ChildItem -File -Filter 'chunk_*.txt' | Sort-Object Name)
if ($chunks.Count -eq 0) { throw 'no chunk_*.txt files; run the batch first' }

$runResults = @{}
$rrPath = Join-Path $root 'run_results.csv'
if (Test-Path $rrPath) {
    Import-Csv $rrPath | ForEach-Object {
        $runResults[[int]$_.run] = $_
    }
}

$stdoutCycle = @{}
Get-ChildItem (Join-Path $root 'runs') -Filter 'run_*.txt' -ErrorAction SilentlyContinue | ForEach-Object {
    $n = 0
    if ($_.BaseName -match 'run_(\d+)') { $n = [int]$Matches[1] }
    $txt = Get-Content -LiteralPath $_.FullName -ErrorAction SilentlyContinue
    $cyc = ''
    $hasDone = $false
    foreach ($line in $txt) {
        if ($line -match '^done:\s+(\d+) cycles') { $cyc = $Matches[1]; $hasDone = $true }
        elseif ($line -match '^\s+(\d+) cycles,') { if (-not $hasDone) { $cyc = $Matches[1] } }
        elseif ($line -match '^cycle (\d+):') { if (-not $hasDone -and -not $cyc) { $cyc = $Matches[1] } }
        elseif ($line -match '^entering cycle loop') { if (-not $cyc) { $cyc = 'entered' } }
    }
    $stdoutCycle[$n] = @{ cycle = $cyc; done = $hasDone }
}

$faults = New-Object System.Collections.Generic.List[object]
$sessionRun = 0
$chunkRunOffset = 0

foreach ($ch in $chunks) {
    $lines = Get-Content -LiteralPath $ch.FullName
    $chunkName = $ch.Name
    $i = 0
    $session = $null

    while ($i -lt $lines.Count) {
        $line = $lines[$i]

        if ($line -match '^===== session (.+), pid (\d+) =====') {
            $sessionRun++
            $session = @{
                run = $sessionRun
                time = $Matches[1]
                pid = $Matches[2]
                chunk = $chunkName
                moduleBases = New-Object System.Collections.Generic.List[uint64]
                moduleNames = @{}
                loads = 0
                saves = 0
                parked = @{}          # tid -> "module path or name"
                parkedInSsCtx = @()
                lastParked = @{}
                nFaults = 0
            }
            $i++
            continue
        }

        if ($null -eq $session) { $i++; continue }

        if ($line -match '^\s+module (\S+)\s+\d+ KB at ([0-9A-Fa-f]+)') {
            $mb = Hex16-ToUInt64 $Matches[2]
            $session.moduleBases.Add($mb) | Out-Null
            $session.moduleNames[$Matches[1]] = $mb
        }
        if ($line -match '^save: slot') { $session.saves++ }
        if ($line -match '^load: slot') { $session.loads++ }
        if ($line -match '^\s+thread (\d+) parked at ([0-9A-Fa-f]+) in (.+)$') {
            $tid = $Matches[1]
            $path = $Matches[3].Trim()
            $leaf = Split-Path $path -Leaf
            $session.parked[$tid] = $leaf
            $session.lastParked[$tid] = $leaf
            if ($leaf -match 'ss_ctx') { $session.parkedInSsCtx += $tid }
        }

        if ($line -match '^fault: ([0-9A-Fa-f]{8}) at ([0-9A-Fa-f]{16}) in (\S+), thread (\d+), (-?\d+) frame\(s\) since the last restore') {
            $session.nFaults++
            $code = $Matches[1].ToUpperInvariant()
            $pc = $Matches[2].ToUpperInvariant()
            $site = $Matches[3]
            $tid = $Matches[4]
            $fsr = [int]$Matches[5]
            $mod = $site
            $off = ''
            if ($site -match '^(.+)\+([0-9A-Fa-f]+)$') { $mod = $Matches[1]; $off = $Matches[2].ToUpperInvariant() }

            $truncated = ($pc.Substring(0, 8) -eq '00000000')

            $f = [ordered]@{
                run = $session.run
                session_time = $session.time
                pid = $session.pid
                chunk = $session.chunk
                loads_before_fault = $session.loads
                saves_before_fault = $session.saves
                code = $code
                pc = $pc
                site = $site
                module = $mod
                offset = $off
                thread = $tid
                frames_since_restore = $fsr
                truncated = [int]$truncated
                interleaved = 0
                regs_trusted = 1
                access_kind = ''
                access_addr = ''
                access_who = ''
                access_who_class = ''
                access_heap = ''
                access_heap_rewind = ''
                faulting_regs = ''
                unwind_depth = 0
                chain = ''
                frames = New-Object System.Collections.Generic.List[string]
                addr_shape = ''
                half_ptr_match = ''
                shape_line = ''
                pattern_line = 0
                stack_base = ''
                stack_limit = ''
                stack_reserve = ''
                rsp = ''
                park_module = ''
                thread_role = ''
                n_faults_in_session = 0
                stdout_cycle = ''
                stdout_done = 0
                run_exit_code = ''
                run_hung = 0
                classified_outcome = ''
                reg_notes = New-Object System.Collections.Generic.List[string]
                n_regs_reported = 0
                n_regs_restored = 0
                n_regs_held = 0
                n_regs_neither = 0
                n_regs_rewind_heap = 0
                n_regs_present_heap = 0
                n_regs_no_heap = 0
            }

            $park = $session.lastParked[$tid]
            if ($park) { $f.park_module = $park }
            $ssCtxTids = @($session.lastParked.Keys | Where-Object { $session.lastParked[$_] -match 'ss_ctx' })
            if ($park -match 'ss_ctx' -and $ssCtxTids.Count -eq 1 -and $ssCtxTids[0] -eq $tid) {
                $f.thread_role = 'requester-likely'
            } elseif ($park -match 'ss_ctx') {
                $f.thread_role = 'ss_ctx-parked'
            } elseif ($park) {
                $f.thread_role = 'restored-other'
            } else {
                $f.thread_role = 'not-in-last-park-list'
            }

            # Collect continuation lines until next fault or session. Track interleaving.
            $j = $i + 1
            $sawFrame0 = $false
            $thisFrames = New-Object System.Collections.Generic.List[string]
            $pendingFaultStarted = $false
            while ($j -lt $lines.Count) {
                $l = $lines[$j]
                if ($l -match '^===== session') { break }
                if ($l -match '^fault: ') {
                    $f.interleaved = 1
                    $f.regs_trusted = 0
                    break
                }

                # A continuation may share a line with an ss_log splice.
                if ($l -match '(reading|writing|executing) ([0-9A-Fa-f]{16}) \(([^)]*)\)') {
                    if (-not $f.access_kind) {
                        $f.access_kind = $Matches[1]
                        $f.access_addr = $Matches[2].ToUpperInvariant()
                        $f.access_who = $Matches[3]
                        $f.access_who_class = Classify-AccessWho $Matches[3]
                        if ($Matches[3] -match 'in the (.+?) heap at ([0-9A-Fa-f]+), which we (rewind|leave in the present)') {
                            $f.access_heap = ($Matches[1] -replace '^the ', '').Trim()
                            $f.access_heap_rewind = $Matches[3]
                        }
                    }
                }
                if ($l -match 'THE FAULTING ADDRESS IS IN ([a-z0-9]+)') {
                    $rn = $Matches[1]
                    if ($f.faulting_regs) { $f.faulting_regs += ';' + $rn } else { $f.faulting_regs = $rn }
                }
                if ($l -match 'SHAPE: (.+)$') {
                    $f.shape_line = $Matches[1].Trim()
                }
                if ($l -match 'PATTERN: the faulting address IS rsp') { $f.pattern_line = 1 }
                if ($l -match 'this thread''s stack: base ([0-9A-Fa-f]+), limit ([0-9A-Fa-f]+), reservation base ([0-9A-Fa-f]+)') {
                    $f.stack_base = $Matches[1].ToUpperInvariant()
                    $f.stack_limit = $Matches[2].ToUpperInvariant()
                    $f.stack_reserve = $Matches[3].ToUpperInvariant()
                }

                if ($l -match '^\s+(rax|rcx|rdx|rbx|rsp|rbp|rsi|rdi|r8|r9|r10|r11|r12|r13|r14|r15)=([0-9A-Fa-f]{16})\s+(.*)$') {
                    $rname = $Matches[1]
                    $rval = $Matches[2].ToUpperInvariant()
                    $rrest = $Matches[3]
                    $info = Parse-RegRest $rrest
                    $f.n_regs_reported++
                    if ($rname -eq 'rsp') { $f.rsp = $rval }
                    if ($info.side -eq 'restored-from-save') { $f.n_regs_restored++ }
                    elseif ($info.side -eq 'held-in-present') { $f.n_regs_held++ }
                    elseif ($info.side -eq 'neither') { $f.n_regs_neither++ }
                    if ($info.heapRewind -eq 'rewind') { $f.n_regs_rewind_heap++ }
                    elseif ($info.heapRewind -eq 'leave in the present') { $f.n_regs_present_heap++ }
                    elseif ($info.heap) { }
                    else { $f.n_regs_no_heap++ }
                    $note = $rname + '=' + $rval + '|' + $info.commit
                    if ($info.heap) { $note += '|heap=' + $info.heap + '/' + $info.heapRewind }
                    if ($info.side) { $note += '|side=' + $info.side }
                    $f.reg_notes.Add($note) | Out-Null
                }

                # Real unwind frames only. Never mix GUESS lines in.
                if ($l -match 'stack-scan GUESS') {
                    # skip
                } elseif ($l -match 'frame (\d+)\s+([0-9A-Fa-f]{16})\s+(.+)$') {
                    $fn = [int]$Matches[1]
                    $faddr = $Matches[2]
                    $flab = $Matches[3].Trim()
                    if ($flab -match 'NOT EXECUTABLE CODE') {
                        $flab = 'NOT_EXECUTABLE'
                    } elseif ($flab -match '(\S+\+[0-9A-Fa-f]+)') {
                        $flab = $Matches[1]
                    }
                    # If we already have a frame 0 and this is another frame 0, the
                    # second belongs to a different interleaved fault — stop.
                    if ($fn -eq 0 -and $sawFrame0) { break }
                    if ($fn -eq 0) { $sawFrame0 = $true }
                    # Only accept in-order frames for this fault (0,1,2,...).
                    if ($fn -eq $thisFrames.Count) {
                        $thisFrames.Add($flab) | Out-Null
                    }
                }

                $j++
            }

            $f.unwind_depth = $thisFrames.Count
            $f.chain = ($thisFrames -join ' > ')
            $f.n_faults_in_session = $session.nFaults

            $rspU = if ($f.rsp) { Hex16-ToUInt64 $f.rsp } else { [uint64]0 }
            $sb = if ($f.stack_base) { Hex16-ToUInt64 $f.stack_base } else { [uint64]0 }
            $sl = if ($f.stack_limit) { Hex16-ToUInt64 $f.stack_limit } else { [uint64]0 }
            $sr = if ($f.stack_reserve) { Hex16-ToUInt64 $f.stack_reserve } else { [uint64]0 }
            $bases = @($session.moduleBases)

            $shapeAddr = $f.access_addr
            if (-not $shapeAddr) { $shapeAddr = $f.pc }
            if ($truncated) {
                $f.addr_shape = 'SET-ASIDE-truncated-pc'
                $f.half_ptr_match = ''
            } else {
                $f.addr_shape = Get-AddrShape $shapeAddr $rspU $bases $sb $sl $sr
                $f.half_ptr_match = Get-HalfPtrMatch $shapeAddr $rspU $bases
                if ($f.pattern_line) {
                    if ($f.half_ptr_match) { $f.half_ptr_match += ';log-PATTERN-rsp-low32-cleared' }
                    else { $f.half_ptr_match = 'log-PATTERN-rsp-low32-cleared' }
                }
            }

            $rr = $runResults[$session.run]
            if ($rr) {
                $f.run_exit_code = $rr.exit_code
                $f.run_hung = [int]$rr.hung
            }
            $sc = $stdoutCycle[$session.run]
            if ($sc) {
                $f.stdout_cycle = [string]$sc.cycle
                $f.stdout_done = [int]$sc.done
            }
            if ($f.run_hung -eq 1) { $f.classified_outcome = 'HUNG' }
            elseif ($f.stdout_done -eq 1) { $f.classified_outcome = 'COMPLETED-WITH-FAULT' }
            else { $f.classified_outcome = 'DIED' }

            $f.reg_summary = ($f.reg_notes -join ';')
            $f.Remove('reg_notes')
            $f.Remove('frames')

            $faults.Add([pscustomobject]$f) | Out-Null
            $i = $j
            continue
        }

        $i++
    }
    $chunkRunOffset = $sessionRun
}

# --- write CSV ---
$cols = @(
    'run','session_time','pid','chunk','classified_outcome','run_exit_code','run_hung','stdout_done','stdout_cycle',
    'loads_before_fault','saves_before_fault','code','pc','site','module','offset','thread','frames_since_restore',
    'truncated','interleaved','regs_trusted','n_faults_in_session',
    'access_kind','access_addr','access_who','access_who_class','access_heap','access_heap_rewind',
    'faulting_regs','unwind_depth','chain','addr_shape','half_ptr_match','shape_line','pattern_line',
    'stack_base','stack_limit','stack_reserve','rsp','park_module','thread_role',
    'n_regs_reported','n_regs_restored','n_regs_held','n_regs_neither','n_regs_rewind_heap','n_regs_present_heap','n_regs_no_heap',
    'reg_summary'
)

$csvPath = Join-Path $root 'crash_faults.csv'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine(($cols -join ','))
foreach ($f in $faults) {
    $vals = foreach ($c in $cols) { Csv-Escape ([string]$f.$c) }
    [void]$sb.AppendLine(($vals -join ','))
}
[System.IO.File]::WriteAllText($csvPath, $sb.ToString(), [Text.UTF8Encoding]::new($false))

# --- aggregates to stdout (the markdown is written separately from these numbers) ---
function Show-Group($title, $items, $key, $total) {
    Write-Host ""
    Write-Host "=== $title ==="
    if (-not $items -or $items.Count -eq 0) { Write-Host "  (none)"; return }
    $items | Group-Object -Property $key | Sort-Object Count -Descending | ForEach-Object {
        $pct = if ($total) { '{0,5:N1}%' -f (100.0 * $_.Count / $total) } else { '' }
        '{0,5}  {1}  {2}' -f $_.Count, $pct, $_.Name
    }
}

# Do not wrap the generic List in @() — Windows PowerShell throws
# "Argument types do not match" on that conversion.
$nAll = $faults.Count
$trunc = @($faults | Where-Object { $_.truncated -eq 1 })
$live = @($faults | Where-Object { $_.truncated -eq 0 })
$nLive = $live.Count
$all = $faults

Write-Host "faults parsed: $nAll"
Write-Host "truncated (set aside for address/offset analysis): $($trunc.Count)"
Write-Host "live (address-shape / site histograms): $nLive"
Write-Host "sessions (runs with a log session): $sessionRun"

Show-Group 'exception code (all faults)' $all 'code' $nAll
Show-Group 'faulting site module+offset (live only)' $live 'site' $nLive
Show-Group 'access kind (live)' $live 'access_kind' $nLive
Show-Group 'access who class (live)' $live 'access_who_class' $nLive
Show-Group 'frames since last restore (all)' $all 'frames_since_restore' $nAll
Show-Group 'unwind depth (all)' $all 'unwind_depth' $nAll
Show-Group 'caller chain (live, real frames only)' $live 'chain' $nLive
Show-Group 'address shape (live)' $live 'addr_shape' $nLive
Show-Group 'half-ptr match notes (live, non-empty)' @($live | Where-Object { $_.half_ptr_match }) 'half_ptr_match' $nLive
Show-Group 'thread role (all)' $all 'thread_role' $nAll
Show-Group 'park module (all)' $all 'park_module' $nAll
Show-Group 'loads before fault = restore index (all)' $all 'loads_before_fault' $nAll
Show-Group 'classified outcome of the run that produced the fault' $all 'classified_outcome' $nAll
Show-Group 'interleaved (all)' $all 'interleaved' $nAll
Show-Group 'module only (live)' $live 'module' $nLive
Show-Group 'access heap (live, non-empty)' @($live | Where-Object { $_.access_heap }) 'access_heap' $nLive

Write-Host ""
Write-Host "=== register-side totals across live, regs_trusted faults ==="
$trusted = @($live | Where-Object { $_.regs_trusted -eq 1 })
$tr = $trusted.Count
Write-Host "trusted-reg faults: $tr of $nLive live"
if ($tr) {
    $sumRest = ($trusted | Measure-Object n_regs_restored -Sum).Sum
    $sumHeld = ($trusted | Measure-Object n_regs_held -Sum).Sum
    $sumNei  = ($trusted | Measure-Object n_regs_neither -Sum).Sum
    $sumRew  = ($trusted | Measure-Object n_regs_rewind_heap -Sum).Sum
    $sumPres = ($trusted | Measure-Object n_regs_present_heap -Sum).Sum
    $sumNoH  = ($trusted | Measure-Object n_regs_no_heap -Sum).Sum
    $sumRep  = ($trusted | Measure-Object n_regs_reported -Sum).Sum
    Write-Host "  reported register values: $sumRep"
    Write-Host "  side restored-from-save: $sumRest"
    Write-Host "  side held-in-present:    $sumHeld"
    Write-Host "  side neither:            $sumNei"
    Write-Host "  in a rewind heap:        $sumRew"
    Write-Host "  in a present heap:       $sumPres"
    Write-Host "  no heap annotation:      $sumNoH"
}

Write-Host ""
Write-Host "wrote $csvPath"
Write-Host "sessions mapped: $sessionRun"
