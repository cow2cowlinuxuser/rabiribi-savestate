# Bake Stage 1 BG plates for GameCube (console-first resolution).
#
# PC: five 2048x1024 walkscape plates + 03_02/03_03 overlays + 06 prop.
# GC: keep STAGE_W = 5 * 1024 world units (section locks unchanged); bake
#     lower-res textures and let GX stretch them to the plate quads.
#
#   powershell -File tools/bake-stage01-bg.ps1            # 640x320 default
#   powershell -File tools/bake-stage01-bg.ps1 -Preset half # 1024x512
#
# Sources: textures/_stage_export + textures/_bg_export
# Outputs: textures/st1_a0.png … st1_a4.png, st1_a2_{far,mid,near}.png, st1_a5.png

param(
  [ValidateSet('console', 'half')]
  [string]$Preset = 'console'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$proj = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path (Join-Path $proj 'textures'))) {
  if (Test-Path 'F:\rbo_fabre_proto\textures') { $proj = 'F:\rbo_fabre_proto' }
  else { throw "Cannot find rbo_fabre_proto/textures" }
}

$tex = Join-Path $proj 'textures'
$stageEx = Join-Path $tex '_stage_export'
$bgEx = Join-Path $tex '_bg_export'

switch ($Preset) {
  'console' { $outW = 640; $outH = 320 }
  'half'    { $outW = 1024; $outH = 512 }
}

function Find-Src([string]$name) {
  foreach ($dir in @($stageEx, $bgEx)) {
    $p = Join-Path $dir $name
    if (Test-Path -LiteralPath $p) { return $p }
  }
  throw "Missing source $name (looked in _stage_export / _bg_export)"
}

function Save-Scaled([string]$srcPath, [string]$dstPath, [int]$w, [int]$h, [bool]$keyBlack) {
  $src = [System.Drawing.Bitmap]::FromFile($srcPath)
  try {
    $work = New-Object System.Drawing.Bitmap $src.Width, $src.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g0 = [System.Drawing.Graphics]::FromImage($work)
    $g0.Clear([System.Drawing.Color]::Transparent)
    $g0.DrawImage($src, 0, 0, $src.Width, $src.Height)
    $g0.Dispose()

    if ($keyBlack) {
      for ($y = 0; $y -lt $work.Height; $y++) {
        for ($x = 0; $x -lt $work.Width; $x++) {
          $c = $work.GetPixel($x, $y)
          if (($c.R + $c.G + $c.B) -lt 24) {
            $work.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(0, 0, 0, 0))
          }
        }
      }
    }

    $out = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($out)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($work, 0, 0, $w, $h)
    $g.Dispose()
    $work.Dispose()

    $out.Save($dstPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $out.Dispose()
    $fi = Get-Item $dstPath
    "{0}: {1}x{2} {3:N1} KB  <- {4}" -f (Split-Path $dstPath -Leaf), $w, $h, ($fi.Length / 1KB), (Split-Path $srcPath -Leaf)
  } finally {
    $src.Dispose()
  }
}

Write-Host "Baking Stage1 BG preset=$Preset (${outW}x${outH}) -> $tex"

# Ground / far plates (opaque CMPR candidates).
Save-Scaled (Find-Src 'BG01_01_01.png') (Join-Path $tex 'st1_a0.png') $outW $outH $false
Save-Scaled (Find-Src 'BG01_02_01.png') (Join-Path $tex 'st1_a1.png') $outW $outH $false
Save-Scaled (Find-Src 'BG01_03_01.png') (Join-Path $tex 'st1_a2_far.png') $outW $outH $false
Save-Scaled (Find-Src 'BG01_04_01.png') (Join-Path $tex 'st1_a3.png') $outW $outH $false
Save-Scaled (Find-Src 'BG01_05_01.png') (Join-Path $tex 'st1_a4.png') $outW $outH $false

# Parallax overlays (alpha). Prefer 03_02 / 03_03; key near black.
Save-Scaled (Find-Src 'BG01_03_02.png') (Join-Path $tex 'st1_a2_mid.png') $outW $outH $true
# 03_03 is often already 1024x512 sky/near strip
Save-Scaled (Find-Src 'BG01_03_03.png') (Join-Path $tex 'st1_a2_near.png') $outW $outH $true

# Prop (PC bg01_06) — keep 256 square.
$propSrc = Find-Src 'BG01_06_01.png'
$propDst = Join-Path $tex 'st1_a5.png'
Copy-Item -LiteralPath $propSrc -Destination $propDst -Force
$b = [System.Drawing.Bitmap]::FromFile($propDst)
"{0}: {1}x{2} (prop copy)" -f 'st1_a5.png', $b.Width, $b.Height
$b.Dispose()

Write-Host @"

World scale (runtime): each plate is 2048 world units (PC pixel width).
  STAGE_W = 5*2048 = 10240. HOST_C.1 locks map 1:1.
  Console ${outW}x${outH} texels stretch onto those quads.

Next:
  cd F:\rbo_fabre_proto
  make sdpack
"@
