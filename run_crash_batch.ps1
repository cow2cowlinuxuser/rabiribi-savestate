# Serial crash-batch runner for ss_ctx.exe. Measurement only: does not rebuild.
# Invokes: ss_ctx.exe 30 4 mono
# Env: D3D9SW_LOCKS=0 D3D9SW_VERIFY=0
# Rotates d3d9_sw_savestate_ss_ctx.txt every 20 runs to avoid the 8 MB truncate.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$exe = Join-Path $root 'ss_ctx.exe'
if (-not (Test-Path $exe)) { throw "ss_ctx.exe not found at $exe" }

$logName = 'd3d9_sw_savestate_ss_ctx.txt'
$logPath = Join-Path $root $logName
$prebatch = Join-Path $root 'd3d9_sw_savestate_ss_ctx.prebatch.txt'
$runsDir = Join-Path $root 'runs'
$resultsPath = Join-Path $root 'run_results.csv'
$chunkAccPath = Join-Path $root 'chunk_accounting.csv'
$progressPath = Join-Path $root 'batch_progress.txt'

New-Item -ItemType Directory -Force -Path $runsDir | Out-Null

# Existing log is from an earlier build: move aside, never include in the dataset.
if (Test-Path $logPath) {
    if (Test-Path $prebatch) {
        $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
        Move-Item -Force $prebatch (Join-Path $root "d3d9_sw_savestate_ss_ctx.prebatch.$stamp.txt")
    }
    Move-Item -Force $logPath $prebatch
}

$env:D3D9SW_LOCKS = '0'
$env:D3D9SW_VERIFY = '0'

$minRuns = 100
$maxRuns = 200
$minCrashes = 25
$chunkSize = 20
$timeoutMs = 150000

'run,exit_code,hung,elapsed_ms,chunk,stdout_bytes,stdout_done' | Set-Content -Encoding UTF8 $resultsPath
'chunk,first_run,last_run,run_count,log_bytes' | Set-Content -Encoding UTF8 $chunkAccPath

$chunkIdx = 1
$chunkFirst = 1
$clean = 0
$died = 0
$hung = 0
$run = 0

function Write-ProgressLine([string]$msg) {
    $line = '{0:yyyy-MM-dd HH:mm:ss}  {1}' -f (Get-Date), $msg
    Add-Content -Encoding UTF8 -Path $progressPath -Value $line
    Write-Host $line
}

function Rotate-Chunk {
    param([int]$LastRun)
    $src = Join-Path $root $logName
    $dst = Join-Path $root ('chunk_{0:D2}.txt' -f $script:chunkIdx)
    if (Test-Path $src) {
        # Engine may still have the file open briefly after exit; retry a few times.
        $moved = $false
        for ($t = 0; $t -lt 10; $t++) {
            try {
                Move-Item -Force $src $dst -ErrorAction Stop
                $moved = $true
                break
            } catch {
                Start-Sleep -Milliseconds 500
            }
        }
        if (-not $moved) { throw "could not rotate $src to $dst" }
        $bytes = (Get-Item $dst).Length
        $n = $LastRun - $script:chunkFirst + 1
        Add-Content -Encoding UTF8 -Path $chunkAccPath -Value ('{0},{1},{2},{3},{4}' -f $script:chunkIdx, $script:chunkFirst, $LastRun, $n, $bytes)
        Write-ProgressLine ("rotated chunk_{0:D2}.txt runs {1}-{2} ({3} bytes)" -f $script:chunkIdx, $script:chunkFirst, $LastRun, $bytes)
        $script:chunkIdx++
        $script:chunkFirst = $LastRun + 1
    } else {
        Write-ProgressLine ("no log to rotate after run {0}" -f $LastRun)
        $script:chunkIdx++
        $script:chunkFirst = $LastRun + 1
    }
}

Write-ProgressLine "batch start: minRuns=$minRuns maxRuns=$maxRuns timeoutMs=$timeoutMs"

while ($true) {
    $run++
    $targetNow = $minRuns
    if ($run -gt $minRuns -and $died -lt $minCrashes) {
        if ($died -lt $minCrashes -and $run -le 150) { $targetNow = 150 }
        if ($died -lt $minCrashes -and $run -gt 150) { $targetNow = $maxRuns }
    }
    if ($run -gt $minRuns) {
        if ($died -ge $minCrashes -or $run -gt $maxRuns) {
            $run--
            break
        }
        if ($run -gt $targetNow) {
            $run--
            break
        }
    }

    Get-ChildItem -Path $root -Filter 'harness_inflight_*.txt' -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    $stdout = Join-Path $runsDir ('run_{0:D3}.txt' -f $run)
    $stderr = Join-Path $runsDir ('run_{0:D3}.err.txt' -f $run)
    if (Test-Path $stdout) { Remove-Item -Force $stdout }
    if (Test-Path $stderr) { Remove-Item -Force $stderr }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList '30','4','mono' -WorkingDirectory $root `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
        -PassThru -WindowStyle Hidden
    # WaitForExit(timeout) does not populate ExitCode; a parameterless WaitForExit
    # after it returns is required, or ExitCode stays empty and clean runs look dead.
    $exited = $p.WaitForExit($timeoutMs)
    $wasHung = $false
    if (-not $exited) {
        $wasHung = $true
        try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch {}
        Start-Sleep -Seconds 5
    }
    try { $p.WaitForExit() } catch {}
    $code = 0
    try { $code = [int]$p.ExitCode } catch { $code = -999 }
    $sw.Stop()
    $elapsed = [int]$sw.ElapsedMilliseconds

    if ($wasHung) { $hung++ }
    elseif ($code -eq 0) { $clean++ }
    else { $died++ }

    $chunkForRun = $chunkIdx
    $outBytes = 0
    $stdoutDone = 0
    if (Test-Path $stdout) {
        $outBytes = (Get-Item $stdout).Length
        if (Select-String -Path $stdout -Pattern '^done:' -Quiet -ErrorAction SilentlyContinue) { $stdoutDone = 1 }
    }
    Add-Content -Encoding UTF8 -Path $resultsPath -Value ('{0},{1},{2},{3},{4},{5},{6}' -f $run, $code, $(if ($wasHung) { 1 } else { 0 }), $elapsed, $chunkForRun, $outBytes, $stdoutDone)

    $tag = if ($wasHung) { 'HUNG' } elseif ($code -eq 0) { 'CLEAN' } else { "DIED $code" }
    Write-ProgressLine ("run {0:D3} {1} exit={2} {3}ms  totals clean={4} died={5} hung={6}" -f $run, $tag, $code, $elapsed, $clean, $died, $hung)

    if (($run % $chunkSize) -eq 0) {
        Rotate-Chunk -LastRun $run
    }

    # After a kill the exe stays file-locked briefly even if we already slept.
    if ($wasHung) { Start-Sleep -Seconds 2 }
}

# Rotate leftover log if the last chunk is partial.
if ($run -ge $chunkFirst) {
    Rotate-Chunk -LastRun $run
}

Write-ProgressLine ("batch done: runs={0} clean={1} died={2} hung={3} chunks={4}" -f $run, $clean, $died, $hung, ($chunkIdx - 1))
'RUNS={0}' -f $run | Set-Content -Encoding UTF8 (Join-Path $root 'batch_summary.txt')
Add-Content -Encoding UTF8 (Join-Path $root 'batch_summary.txt') -Value ('CLEAN={0}' -f $clean)
Add-Content -Encoding UTF8 (Join-Path $root 'batch_summary.txt') -Value ('DIED={0}' -f $died)
Add-Content -Encoding UTF8 (Join-Path $root 'batch_summary.txt') -Value ('HUNG={0}' -f $hung)
