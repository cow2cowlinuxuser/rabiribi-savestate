# Reads the growth counters out of the D3D11 log.
#
# The live-resource count alone could not settle what was happening: in one
# session it climbed from 225 to 12509 and also fell back to 225 more than once,
# which is equally consistent with a leak and with heavy churn. born minus died
# is the number of wrapper objects nothing has released, and the retain columns
# say how much is being held deliberately for a snapshot. Those two separate the
# cases; the live count does not.
#
#   .\leak.ps1          # last 20 samples
#   .\leak.ps1 -N 200

param(
    [int]$N = 20,
    [string]$Log = 'C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi\d3d11_sw.log'
)

if (-not (Test-Path $Log)) { Write-Host "no log at $Log"; exit 1 }

$re = 'perf mem: (\d+) live resources, ([\d.]+) MB of payload \| born (\d+) died (\d+) unretained-live (\d+) \| retained (\d+) now, (\d+) pushed, (\d+) post-save skipped, (\d+) flush\(es\) \| VA ([\d.]+) MB used, ([\d.]+) MB free'
$rows = @()
foreach ($line in Get-Content $Log) {
    if ($line -match $re) {
        $rows += [pscustomobject]@{
            live     = [int]$matches[1]
            payloadMB = [double]$matches[2]
            born     = [int]$matches[3]
            died     = [int]$matches[4]
            unretlive = [int]$matches[5]
            retained = [int]$matches[6]
                pushed   = [int]$matches[7]
                skipped  = [int]$matches[8]
                flushes  = [int]$matches[9]
                VAusedMB = [double]$matches[10]
                VAfreeMB = [double]$matches[11]
        }
    }
}

if (-not $rows) {
    Write-Host 'No samples with the new counters yet.'
    Write-Host 'The old format is still being written, so this log predates the'
    Write-Host 'instrumented build. Run the game once and try again.'
    exit
}

$rows | Select-Object -Last $N | Format-Table -AutoSize

$f = $rows[0]; $l = $rows[-1]
Write-Host ''
Write-Host ("over {0} samples: unretained-live {1} -> {2}, retained {3} -> {4}, VA free {5:N0} -> {6:N0} MB" -f `
    $rows.Count, $f.unretlive, $l.unretlive, $f.retained, $l.retained, $f.VAfreeMB, $l.VAfreeMB)
Write-Host ("post-save releases declined: {0}  (these are the ones that used to be kept for nothing)" -f $l.skipped)
Write-Host ''
Write-Host ("born - died = {0}, and live = {1}. These should agree; a gap means an object" -f ($l.born - $l.died), $l.live)
Write-Host 'was neither freed nor retained, which is the only true leak this table can show.'
Write-Host ''
Write-Host 'unretained-live  -> the working set, NOT orphans. Rises slowly if restores strand objects.'
Write-Host 'retained         -> held back for a snapshot; only a superseding save empties it.'
Write-Host 'skipped          -> declined because the object postdates every save, so nothing can name it.'
Write-Host 'born/died apart  -> the real leak. Together, with live flat, is healthy churn.'

