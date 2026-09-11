<#
.SYNOPSIS
  Crop GC face buttons (A/B/X/Y large+small) from 159596.png into textures/gc_pad_icons.png.
  Source layout measured from connected components (teal #008080 bg, transparent frames).
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$srcPath = 'F:\rbo_fabre_proto\159596.png'
$outPath = 'F:\rbo_fabre_proto\textures\gc_pad_icons.png'
$metaPath = 'F:\rbo_fabre_proto\textures\gc_pad_icons_meta.txt'

# Measured crops (x,y,w,h) on 447x495 sheet — large + small face buttons only.
$crops = [ordered]@{
  'A'    = @{ X = 5;   Y = 246; W = 40; H = 40 }
  'B'    = @{ X = 53;  Y = 246; W = 40; H = 40 }
  'X'    = @{ X = 101; Y = 245; W = 28; H = 45 }
  'Y'    = @{ X = 137; Y = 245; W = 39; H = 26 }
  'A_SM' = @{ X = 64;  Y = 356; W = 19; H = 19 }
  'B_SM' = @{ X = 106; Y = 354; W = 20; H = 20 }
  'X_SM' = @{ X = 183; Y = 352; W = 13; H = 22 }
  'Y_SM' = @{ X = 210; Y = 353; W = 22; H = 15 }
}

function Test-Bg([Drawing.Color]$c) {
  if ($c.A -lt 16) { return $true }
  return ($c.R -lt 40 -and $c.G -gt 100 -and $c.G -lt 160 -and $c.B -gt 100 -and $c.B -lt 160)
}

$bmp = New-Object Drawing.Bitmap $srcPath
Write-Host ("Source {0}x{1}" -f $bmp.Width, $bmp.Height)

$sheetW = 256
$sheetH = 128
$cellW = 64
$cellH = 64
$out = New-Object Drawing.Bitmap $sheetW, $sheetH, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
for ($yy = 0; $yy -lt $sheetH; $yy++) {
  for ($xx = 0; $xx -lt $sheetW; $xx++) {
    $out.SetPixel($xx, $yy, [Drawing.Color]::FromArgb(0, 0, 0, 0))
  }
}

$order = @(
  @{ Key = 'A'; Col = 0; Row = 0 },
  @{ Key = 'B'; Col = 1; Row = 0 },
  @{ Key = 'X'; Col = 2; Row = 0 },
  @{ Key = 'Y'; Col = 3; Row = 0 },
  @{ Key = 'A_SM'; Col = 0; Row = 1 },
  @{ Key = 'B_SM'; Col = 1; Row = 1 },
  @{ Key = 'X_SM'; Col = 2; Row = 1 },
  @{ Key = 'Y_SM'; Col = 3; Row = 1 }
)

$meta = New-Object System.Collections.Generic.List[string]
$meta.Add("SHEET ${sheetW}x${sheetH}")
$uv = New-Object System.Collections.Generic.List[string]

foreach ($o in $order) {
  $p = $crops[$o.Key]
  $dx = $o.Col * $cellW
  $dy = $o.Row * $cellH
  $ox = [Math]::Max(0, [int](($cellW - $p.W) / 2))
  $oy = [Math]::Max(0, [int](($cellH - $p.H) / 2))
  for ($yy = 0; $yy -lt $p.H; $yy++) {
    for ($xx = 0; $xx -lt $p.W; $xx++) {
      $c = $bmp.GetPixel(($p.X + $xx), ($p.Y + $yy))
      $tx = $dx + $ox + $xx
      $ty = $dy + $oy + $yy
      if ($tx -ge $sheetW -or $ty -ge $sheetH) { continue }
      if (Test-Bg $c) {
        $out.SetPixel($tx, $ty, [Drawing.Color]::FromArgb(0, 0, 0, 0))
      } else {
        $a = if ($c.A -lt 200) { $c.A } else { 255 }
        if ($a -lt 16) {
          $out.SetPixel($tx, $ty, [Drawing.Color]::FromArgb(0, 0, 0, 0))
        } else {
          $out.SetPixel($tx, $ty, [Drawing.Color]::FromArgb($a, $c.R, $c.G, $c.B))
        }
      }
    }
  }
  $sx = $dx + $ox
  $sy = $dy + $oy
  $meta.Add(("{0} src=({1},{2},{3},{4}) atlas=({5},{6},{3},{4})" -f `
    $o.Key, $p.X, $p.Y, $p.W, $p.H, $sx, $sy))
  $uv.Add(("{0} {1} {2} {3} {4}" -f $o.Key, $sx, $sy, $p.W, $p.H))
  Write-Host ("{0}: atlas ({1},{2}) {3}x{4}" -f $o.Key, $sx, $sy, $p.W, $p.H)
}

$bmp.Dispose()
$out.Save($outPath, [Drawing.Imaging.ImageFormat]::Png)
$out.Dispose()
$meta | Set-Content -Path $metaPath -Encoding UTF8
$uv | Set-Content -Path 'F:\rbo_fabre_proto\textures\gc_pad_icons_uv.txt' -Encoding UTF8
Write-Host "Wrote $outPath"
