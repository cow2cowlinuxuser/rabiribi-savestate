# One line per session, so runs can be compared instead of remembered.
#
# The savestate log is append-only and now runs to well over a megabyte across
# forty sessions. Every question worth asking of it - did the tear happen, did
# the patch hold, where did the runaways land - has been answered so far by
# grepping the whole file and then working out by hand which session the hits
# belonged to. That is how the 256-buffer dsound cap went unnoticed for days,
# and how three builds in a row were judged on a line that was never printed.
#
#   .\sessions.ps1        # the last 8
#   .\sessions.ps1 -N 40  # everything

param(
    [int]$N = 8,
    [string]$Log = 'C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi\d3d9_sw_savestate_rabiribi.txt'
)

if (-not (Test-Path $Log)) { Write-Host "no log at $Log"; exit 1 }

$lines = Get-Content $Log
$starts = @($lines | Select-String '^===== session ' | ForEach-Object { $_.LineNumber })
if (-not $starts) { Write-Host 'no sessions in the log'; exit 1 }

$rows = @()
for ($i = 0; $i -lt $starts.Count; $i++) {
    $from = $starts[$i] - 1
    $to = if ($i + 1 -lt $starts.Count) { $starts[$i + 1] - 2 } else { $lines.Count - 1 }
    if ($to -lt $from) { continue }
    $body = $lines[$from..$to]
    $text = $body -join "`n"

    $when = if ($body[0] -match 'session (\S+ \S+)') { $matches[1] } else { '?' }

    # Where the runaways landed matters more than how many there were: the whole
    # point of the one-byte patch was to move them off +6E9F8.
    $sites = @([regex]::Matches($text, 'runaway: block \w+ of -?\d+ byte\(s\) at [0-9A-F]+ \((rabiribi\.exe\+[0-9A-F]+)\)') |
               ForEach-Object { $_.Groups[1].Value } | Group-Object |
               ForEach-Object { "$($_.Name.Replace('rabiribi.exe+',''))x$($_.Count)" }) -join ' '

    $rows += [pscustomobject]@{
        when     = $when
        # By the presence of dsound's threads, not by our own dsound: log lines.
        # Those lines only exist in builds that have dsoundhook.c, so using them
        # reported every older session as silent - including one that had a
        # runaway in the codec path, which audio-off cannot produce.
        audio    = if ($text -match 'dsound\.dll') { 'on' } else { 'OFF' }
        # 'load: slot' is one completed restore. The first version counted
        # 'save cost ms', which is printed on some saves and not others, and so
        # reported a session with 43 restores as having 2 - hiding the best run
        # the project has had behind a number that looked like a failure.
        saves    = ([regex]::Matches($text, 'verify at save')).Count
        restores = ([regex]::Matches($text, '(?m)^\s*load: slot')).Count
        tore     = ([regex]::Matches($text, 'CHANGED while we copied')).Count
        patch    = if ($text -match 'patch APPLIED') { 'yes' }
                   elseif ($text -match 'decoder:') { 'no' } else { '-' }
        runaways = if ($sites) { $sites } else { '-' }
        faults   = ([regex]::Matches($text, "(?m)^\s*fault: ")).Count
        froze    = ([regex]::Matches($text, 'FROZEN')).Count
    }
}

$rows | Select-Object -Last $N | Format-Table -AutoSize

Write-Host ''
Write-Host 'tore = regions that changed while every thread was suspended.'
Write-Host 'runaways are listed by site, because which site they hit is the finding.'
