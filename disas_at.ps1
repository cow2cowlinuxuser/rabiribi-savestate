# Read the code at a given offset in rabiribi.exe, from memory rather than disk.
#
# The file on disk is encrypted by Steam's DRM stub, so a disassembler pointed at
# it produces nothing usable - that is the same wall the import-table script hit.
# The decrypted code only exists inside a running process, so the game has to be
# up. Nothing is written to the target and no breakpoints are set: this attaches
# non-invasively, reads, and detaches.
#
# Default target is the block copy that computes a negative length. The audio
# harness ruled out the play cursor as the source of that length, so the question
# now is what the length is actually computed FROM - which is a question the
# instructions ahead of the copy can answer directly.
#
#   .\disas_at.ps1                 # rabiribi.exe+6E9F8
#   .\disas_at.ps1 -Offset 36913A  # the other site that faulted

param(
    # Several at once, because a cluster of addresses in one subsystem is read
    # together or not understood at all - and because the game has to be running
    # for any of them, so a second pass costs a relaunch.
    [string[]]$Offset = @('6E9F8'),
    [string]$Process = 'rabiribi',
    [int]$Back = 40,
    [int]$Fwd = 24
)

$ErrorActionPreference = 'Stop'

$proc = Get-Process $Process -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) {
    Write-Host "$Process is not running."
    Write-Host "Steam decrypts the code into memory at launch, so it can only be read"
    Write-Host "while the game is up. Launch it and run this again."
    exit 1
}

$cdb = @(
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe',
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cdb) { Write-Host 'cdb.exe not found.'; exit 1 }

# The symbol server is what let the earlier heap commands resolve ntdll types.
# It costs nothing here and makes any ntdll frames readable.
if (-not $env:_NT_SYMBOL_PATH) {
    $env:_NT_SYMBOL_PATH = 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols'
}

# ub walks backwards through variable-length x86, which cannot be done by simply
# subtracting bytes - start mid-instruction and the whole listing is garbage.
$cmds = @(".echo ===== module =====", "lm m $Process")
foreach ($o in $Offset) {
    $t = "$Process+$o"
    $cmds += ".echo ===== $o : leading up to it ====="
    $cmds += "ub $t L$Back"
    $cmds += ".echo ===== $o : at and after ====="
    $cmds += "u $t L$Fwd"
}
$cmds += ".echo ===== done ====="
$cmds += "qd"
$script = $cmds -join '; '

Write-Host "attaching non-invasively to pid $($proc.Id), reading $($Offset -join ', ')"
Write-Host ''

$out = & $cdb -pv -p $proc.Id -c $script 2>&1 | Out-String

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$path = Join-Path $PSScriptRoot "disas-$($Offset[0])-$stamp.txt"
Set-Content -Path $path -Value $out -NoNewline

# Trim cdb's banner so the interesting part is what appears on screen.
$lines = $out -split "`r?`n"
$start = ($lines | Select-String -Pattern '===== module' | Select-Object -First 1).LineNumber
if ($start) { $lines = $lines[($start - 1)..($lines.Count - 1)] }
$lines | ForEach-Object { $_.TrimEnd() }

Write-Host ''
Write-Host "full output: $path"
