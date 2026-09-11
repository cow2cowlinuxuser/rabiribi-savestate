# Dumps everything that differs between two machines running the wrapper.
# Run on both, diff the output. Build identity is first because a mismatched
# DLL explains more crashes than any setting does.

$game = "C:\Program Files (x86)\Steam\steamapps\common\DoDonPachi Resurrection"

"=== build identity ==="
foreach ($f in @("d3d9.dll", "default.exe")) {
    $p = Join-Path $game $f
    if (Test-Path $p) {
        $i = Get-Item $p
        $h = (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0, 16)
        "{0,-14} {1,10} bytes  {2}  sha {3}" -f $f, $i.Length, $i.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"), $h
    } else {
        "{0,-14} MISSING" -f $f
    }
}

"`n=== wrapper settings (effective) ==="
# name, default when unset
$vars = @(
    @("D3D9_SW_TRACE",            "on"),
    @("D3D9SW_PROF",              "off"),
    @("D3D9SW_CLIENT",            "native"),
    @("D3D9SW_FILTER",            "game's choice"),
    @("D3D9SW_SUBPIXEL",          "4"),
    @("D3D9SW_NOSIMD",            "SIMD on"),
    @("D3D9SW_GATHER",            "auto"),
    @("D3D9SW_THREADS",           "auto, capped"),
    @("D3D9SW_REWIND_CLOCK",      "off"),
    @("D3D9SW_REWIND_EVENTS",     "off"),
    @("D3D9SW_REWIND_THREADS",    "all threads"),
    @("D3D9SW_REWIND_NEWTHREADS", "refuse"),
    @("D3D9SW_REWIND_RECLAIM",    "off"),
    @("D3D9SW_REWIND_GUARD",      "off")
)
foreach ($v in $vars) {
    $set = [Environment]::GetEnvironmentVariable($v[0], "Process")
    if (-not $set) { $set = [Environment]::GetEnvironmentVariable($v[0], "User") }
    if (-not $set) { $set = [Environment]::GetEnvironmentVariable($v[0], "Machine") }
    if ($set) { "{0,-26} = {1}" -f $v[0], $set }
    else      { "{0,-26}   (unset -> {1})" -f $v[0], $v[1] }
}

"`n=== machine ==="
$cs = Get-CimInstance Win32_ComputerSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem
"cpu            {0}" -f $cpu.Name.Trim()
"cores          {0} physical, {1} logical" -f $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors
"ram            {0:N1} GB" -f ($cs.TotalPhysicalMemory / 1GB)
"windows        {0} build {1}" -f $os.Caption, $os.BuildNumber
"pagefile       {0:N1} GB committed limit" -f ((Get-CimInstance Win32_OperatingSystem).TotalVirtualMemorySize / 1MB)

"`n=== dlls the rewind has to work around ==="
foreach ($d in @("XAudio2_7.dll", "DINPUT8.dll", "combase.dll", "inputhost.dll", "winmm.dll", "MSVCR100.dll")) {
    $f = Get-ChildItem -Path "$env:SystemRoot\SysWOW64\$d", "$game\$d" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { "{0,-16} {1}" -f $d, $f.VersionInfo.FileVersion }
    else    { "{0,-16} not found in SysWOW64 or game dir" -f $d }
}

"`n=== steam ==="
$ov = "unknown"
try {
    $c = Get-Content "$env:ProgramFiles(x86)\Steam\config\config.vdf" -ErrorAction Stop -Raw
    if ($c -match '"EnableGameOverlay"\s*"(\d)"') { $ov = if ($Matches[1] -eq "1") { "enabled" } else { "disabled" } }
} catch { }
"game overlay   $ov"
"gameoverlay loaded in this session is what matters; check the savestate log's exclude line"
