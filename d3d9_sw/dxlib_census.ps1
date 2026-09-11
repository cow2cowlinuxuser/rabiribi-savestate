# Census of the DxLib code inside rabiribi.exe.
#
# DxLib is statically linked, so there is no import table to read and no export
# table to walk - the library is melted into the game's own .text. Steam's DRM
# stub then encrypts that on disk, so the file on disk tells us nothing either.
# The only copy that is both decrypted and complete is the one in memory of a
# running process, which is why this attaches rather than reads the exe.
#
# Attaches non-invasively (-pv), so the game keeps running and never learns it
# was inspected. Nothing here writes to the target.
#
# What it collects:
#   1. The module base, so every offset in our logs can be turned into an
#      address and back again across runs.
#   2. Strings from the decrypted image. DxLib carries its own diagnostics, and
#      those strings name functions and source files that the stripped binary
#      otherwise hides.
#   3. Disassembly of every game offset our savestate logs have ever attributed
#      a read or a write to, which is the set that actually touches state we
#      rewind.

param(
    [string]$Exe = "rabiribi.exe",
    [string]$Out = "F:\rbo_fabre_proto\d3d9_sw\dxlib_census.txt"
)

$ErrorActionPreference = "Stop"

$cdb = @(
    "C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe",
    "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cdb) { throw "cdb.exe not found - install the Windows SDK Debugging Tools" }

$proc = Get-Process ($Exe -replace '\.exe$','') -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { throw "$Exe is not running - start the game first, this reads the decrypted image from memory" }

# Offsets our own logs have attributed to game code. These are the sites that
# read or wrote memory a restore had just put back, so they are the DxLib entry
# points that matter to us rather than an arbitrary sample of the library.
$offsets = @(
    '71dc0',   # contains 71e23; walks the handle list
    '71e23',   # DxLib SubHandle - zeroes the pool sentinel, our original suspect
    '71ed0',   # wrote into the node pool
    '2c110', '20bd35', 'bde7e', 'fa18d', '5e49d',
    'ba151', '2c72f0', '2bd465', '1170', '4f520', '6f533', '6e9f8'
)

$mod = $Exe -replace '\.exe$',''
$cmds = @("lm m $mod", ".echo ===== BASE =====", "? $mod")
foreach ($o in $offsets) {
    $cmds += ".echo ===== OFFSET $o ====="
    $cmds += "uf $mod+$o"
}
# DxLib's own diagnostic strings, which name its functions and source files.
$cmds += ".echo ===== STRINGS (ascii) ====="
$cmds += "s -a $mod L?0x400000 `"Dx`""
$cmds += ".echo ===== STRINGS (cpp source names) ====="
$cmds += "s -a $mod L?0x400000 `".cpp`""
$cmds += "q"

$script = ($cmds -join "; ")
Write-Host "attaching to pid $($proc.Id) non-invasively..."
& $cdb -pv -p $proc.Id -c $script 2>&1 | Out-File -FilePath $Out -Encoding ascii

Write-Host "wrote $Out"
$txt = Get-Content $Out
"  lines: $($txt.Count)"
"  functions disassembled: $((($txt | Select-String '^===== OFFSET').Count))"
"  Dx strings: $((($txt | Select-String 'Dx[A-Za-z_]{3,}').Count))"
"  .cpp names: $((($txt | Select-String '\.cpp').Count))"
