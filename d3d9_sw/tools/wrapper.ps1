# Swap the software wrapper in and out of a game folder for A/B testing.
#
# Comparing our renderer against the real one is only meaningful if the only
# thing that changes between runs is the renderer, so move the DLLs rather than
# rebuilding or reinstalling, and say plainly which state the folder is in.
#
#   .\tools\wrapper.ps1 status
#   .\tools\wrapper.ps1 off      # run on the real d3d11
#   .\tools\wrapper.ps1 on       # run on ours again
#
# Defaults to Rabi-Ribi; pass -Game for anything else.

param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'on', 'off')]
    [string]$Action = 'status',

    [string]$Game = 'C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi'
)

$names = @('d3d11.dll', 'dxgi.dll')

if (-not (Test-Path $Game)) { throw "game folder not found: $Game" }
if (Get-Process -Name rabiribi -ErrorAction SilentlyContinue) {
    throw 'the game is running - close it first, or the files are locked'
}

function State {
    $live = @($names | Where-Object { Test-Path (Join-Path $Game $_) }).Count
    $off  = @($names | Where-Object { Test-Path (Join-Path $Game "$_.off") }).Count
    if ($live -eq $names.Count -and $off -eq 0) { return 'on' }
    if ($off -eq $names.Count -and $live -eq 0) { return 'off' }
    return "mixed (live=$live disabled=$off)"
}

switch ($Action) {
    'status' {
        "wrapper is $(State) in $Game"
        # The log is the only record of what the last run did, so date it.
        $log = Join-Path $Game 'd3d11_sw.log'
        if (Test-Path $log) {
            $i = Get-Item $log
            "last log: {0:n0} bytes, written {1}" -f $i.Length, $i.LastWriteTime
        } else {
            'last log: none'
        }
    }
    'off' {
        foreach ($n in $names) {
            $p = Join-Path $Game $n
            if (Test-Path $p) { Move-Item -Force $p "$p.off" }
        }
        # Renaming the log too, so a crash on the real driver cannot be
        # mistaken for one of ours by leaving a stale log beside it.
        $log = Join-Path $Game 'd3d11_sw.log'
        if (Test-Path $log) { Move-Item -Force $log (Join-Path $Game 'd3d11_sw.log.ours') }
        "wrapper is now $(State) - this run uses the real d3d11"
    }
    'on' {
        foreach ($n in $names) {
            $p = Join-Path $Game "$n.off"
            if (Test-Path $p) { Move-Item -Force $p (Join-Path $Game $n) }
        }
        "wrapper is now $(State)"
    }
}
