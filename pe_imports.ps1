# Dump a PE's import table.
#
# Answers one question: does the game reach the C runtime through an import
# table we could patch, or does it link the CRT statically? If malloc/free come
# from ucrtbase by name, patch_iat has a site to redirect and the allocator
# hook is viable. If the imports name no C runtime at all, there is nothing to
# patch and the only hook left is an inline one on ntdll itself.
#
# Written by hand because there is no dumpbin or llvm-readobj on this machine.

param([Parameter(Mandatory = $true)][string]$Path)

$b = [IO.File]::ReadAllBytes($Path)
function U16($o) { [BitConverter]::ToUInt16($b, $o) }
function U32($o) { [BitConverter]::ToUInt32($b, $o) }

if ((U16 0) -ne 0x5A4D) { "not a PE: bad MZ"; exit 1 }
$pe = U32 0x3C
if ((U32 $pe) -ne 0x4550) { "not a PE: bad signature"; exit 1 }

$nSect    = U16 ($pe + 6)
$optSize  = U16 ($pe + 20)
$opt      = $pe + 24
$magic    = U16 $opt
$plus     = ($magic -eq 0x20B)
Write-Host ("format      : {0}" -f $(if ($plus) { "PE32+ (64-bit)" } else { "PE32 (32-bit)" }))

# The data directory sits at a different offset in PE32 vs PE32+, and the
# import table is entry 1.
$ddOff = $opt + $(if ($plus) { 112 } else { 96 })
$impRva  = U32 $ddOff
$impSize = U32 ($ddOff + 4)
Write-Host ("import dir  : rva 0x{0:X} size 0x{1:X}" -f $impRva, $impSize)
if ($impRva -eq 0) { "NO IMPORT TABLE - packed or fully static"; exit }

# Section table follows the optional header; needed to turn RVAs into offsets.
$secBase = $opt + $optSize
$sections = @()
for ($i = 0; $i -lt $nSect; $i++) {
    $s = $secBase + ($i * 40)
    $sections += [pscustomobject]@{
        Name = ([Text.Encoding]::ASCII.GetString($b, $s, 8)).Trim([char]0)
        Va   = U32 ($s + 12); VSize = U32 ($s + 8)
        Raw  = U32 ($s + 20); RSize = U32 ($s + 16)
    }
}
Write-Host "sections    : $(($sections | ForEach-Object { $_.Name }) -join ', ')"

function RvaToOff($rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.Va -and $rva -lt ($s.Va + [Math]::Max($s.VSize, $s.RSize))) {
            return $s.Raw + ($rva - $s.Va)
        }
    }
    return -1
}
function AsciiAt($off) {
    if ($off -lt 0 -or $off -ge $b.Length) { return "<bad>" }
    $e = $off; while ($e -lt $b.Length -and $b[$e] -ne 0) { $e++ }
    return [Text.Encoding]::ASCII.GetString($b, $off, $e - $off)
}

Write-Host ""
$d = RvaToOff $impRva
if ($d -lt 0) { "import dir RVA does not map into any section - packed"; exit }

$crt = 0
while ($true) {
    $oft = U32 $d; $nameRva = U32 ($d + 12); $ft = U32 ($d + 16)
    if ($oft -eq 0 -and $nameRva -eq 0 -and $ft -eq 0) { break }
    $dll = AsciiAt (RvaToOff $nameRva)
    # Prefer the original thunk array: the bound one may hold addresses.
    $thunkRva = $(if ($oft -ne 0) { $oft } else { $ft })
    $t = RvaToOff $thunkRva
    $names = @()
    if ($t -ge 0) {
        while ($true) {
            $v = U32 $t
            if ($v -eq 0) { break }
            if (($v -band 0x80000000) -eq 0) {
                $ho = RvaToOff $v
                if ($ho -ge 0) { $names += AsciiAt ($ho + 2) }
            } else {
                $names += ("#" + ($v -band 0xFFFF))
            }
            $t += 4
        }
    }
    $isCrt = $dll -match 'ucrtbase|msvcr|vcruntime|api-ms-win-crt'
    if ($isCrt) { $crt++ }
    Write-Host ("{0,-34} {1,4} import(s){2}" -f $dll, $names.Count, $(if ($isCrt) { "   <-- C RUNTIME" } else { "" }))
    $alloc = $names | Where-Object { $_ -match '^(malloc|calloc|realloc|free|_msize|_expand|\?\?2@|\?\?3@|\?\?_U|\?\?_V|new|delete)' }
    if ($alloc) { Write-Host ("      allocation entries: " + (($alloc | Select-Object -First 12) -join ', ')) }
    $d += 20
}

Write-Host ""
if ($crt -eq 0) {
    Write-Host "VERDICT: imports name no C runtime. Either the CRT is static or the"
    Write-Host "         import table is the DRM stub's rather than the game's."
} else {
    Write-Host "VERDICT: the CRT is imported by name, so there are IAT sites to patch."
}
