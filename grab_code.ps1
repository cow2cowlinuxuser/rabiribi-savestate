# Reads decrypted game code out of the live process.
#
# The executable is Steam-DRM encrypted on disk, so a static disassembler sees
# only the stub. The real instructions exist solely in memory after the stub has
# run, which means the only way to look at them is to attach to a running game.
#
# Attaches NON-INVASIVELY (-pv): cdb opens the process and reads memory without
# calling DebugActiveProcess, so IsDebuggerPresent stays false and no debug port
# is created. The game is suspended while the read happens and continues when
# cdb detaches. Nothing is written to the process.

$cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"
$out = "F:\rbo_fabre_proto\d3d9_sw\rabiribi_code.txt"

# The addresses the guard pages named, as offsets from the rabiribi.exe base.
# 71e23 is the one that matters: it read the second pointer of the list head
# that reverts, in two separate sessions.
$targets = @(
  @{ off = "71dc0"; n = "60"; why = "the function containing +71e23, which reads the reverting list head" },
  @{ off = "71ed0"; n = "30"; why = "+71ed9, which wrote into the node pool" },
  @{ off = "bde7e"; n = "20"; why = "wrote 016A28A8 in the latest run" },
  @{ off = "20bd35"; n = "20"; why = "wrote 016B3530 in both runs" },
  @{ off = "2c110"; n = "18"; why = "read 0117EA60 in both runs" }
)

Write-Host "waiting for rabiribi.exe..."
$p = $null
for ($i = 0; $i -lt 600; $i++) {
    $p = Get-Process rabiribi -ErrorAction SilentlyContinue
    if ($p) { break }
    Start-Sleep -Seconds 2
}
if (-not $p) { "TIMED OUT waiting for the game" | Tee-Object $out; exit 1 }

Write-Host "found pid $($p.Id); letting the Steam stub decrypt and the game settle"
Start-Sleep -Seconds 25

# uf walks a whole function and is the better answer when it works; u is the
# fallback for when there are no symbols to find the boundaries with.
$cmds = @("lm m rabiribi", ".echo ---- FUNCTION AT 71e23 ----", "uf rabiribi+71e23")
foreach ($t in $targets) {
    $cmds += ".echo ---- $($t.off) : $($t.why) ----"
    $cmds += "u rabiribi+$($t.off) L$($t.n)"
}
$cmds += "q"

& $cdb -pv -p $p.Id -c ($cmds -join "; ") 2>&1 |
    Where-Object { $_ -notmatch 'Repository|Preparing the env|Waiting for Debug|Copyright|Microsoft \(R\)|Path validation|Deferred|^\s*$|ExtensionRepository|Nuget|EnableRedirect|Configuring repos|search path' } |
    Set-Content $out

Write-Host "wrote $out"
