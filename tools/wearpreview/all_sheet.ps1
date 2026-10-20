# R5c: previews of every colour/normal pair of catalog\tire_regions.csv with
# its tread rectangle, and contact sheets of the current look at 0, 57 and
# 100% (one repetition of the tread tile each), 3 tires per row, 4 rows per
# sheet. Read-only on the catalog and the extracted textures.
param(
    [string]$Project = (Resolve-Path "$PSScriptRoot\..\..").Path
)
$tool = "$Project\tools\wearpreview\wearpreview.exe"
$out = "$Project\probe\previews\r5d\all"
New-Item -ItemType Directory -Force $out | Out-Null
$roots = @{
    'base_vehicle' = "$Project\probe\extracted"
    'dlc_goodyear' = "$Project\probe\extracted_dlc_goodyear"
    'dlc_michelin' = "$Project\probe\extracted_dlc_michelin"
}
$items = @()
Import-Csv "$Project\catalog\tire_regions.csv" | ForEach-Object {
    $src, $crel = $_.color -split ':', 2
    $nrel = ($_.normal -split ':', 2)[1]
    $c = Join-Path $roots[$src] ($crel.TrimStart('/') -replace '/', '\')
    $n = Join-Path $roots[$src] ($nrel.TrimStart('/') -replace '/', '\')
    if (-not (Test-Path $c) -or -not (Test-Path $n)) { Write-Output "MISSING $c"; return }
    $name = [IO.Path]::GetFileNameWithoutExtension($c) -replace '_colspec$|_c(\.dlc_[a-z]+)?$', ''
    & $tool $c $n "$out\$name" $_.u0 $_.v0 $_.u1 $_.v1 | Out-Null
    $items += $name
}
Add-Type -AssemblyName System.Drawing
$cellH = 260; $gap = 6; $perRow = 3; $rowsPerSheet = 4; $head = 16
$font = New-Object System.Drawing.Font 'Arial', 9
$sheets = [math]::Ceiling($items.Count / ($perRow * $rowsPerSheet))
for ($s = 0; $s -lt $sheets; $s++) {
    $bmp = New-Object System.Drawing.Bitmap 1500, ($rowsPerSheet * ($cellH + $head + $gap))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $g.InterpolationMode = 'HighQualityBicubic'
    for ($k = 0; $k -lt $perRow * $rowsPerSheet; $k++) {
        $i = $s * $perRow * $rowsPerSheet + $k
        if ($i -ge $items.Count) { break }
        $img = [System.Drawing.Image]::FromFile("$out\$($items[$i])_compare.bmp")
        # compare.bmp: 5 cells of width tw (+12 gap), rows of 2 tiles (+12 gap)
        $tw = ($img.Width - 48) / 5; $th = ($img.Height - 12) / 4
        $cellW = [int]($cellH * $tw / $th)
        $ox = ($k % $perRow) * 500; $oy = [math]::Floor($k / $perRow) * ($cellH + $head + $gap)
        $g.DrawString($items[$i], $font, [System.Drawing.Brushes]::Black, [float]$ox, [float]$oy)
        $j = 0
        foreach ($col in 0, 2, 4) {
            $src = New-Object System.Drawing.RectangleF ([float]($col * ($tw + 12))), ([float](2 * $th + 12)), ([float]$tw), ([float]$th)
            $dst = New-Object System.Drawing.RectangleF ([float]($ox + $j * ($cellW + 4))), ([float]($oy + $head)), ([float]$cellW), ([float]$cellH)
            $g.DrawImage($img, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
            $j++
        }
        $img.Dispose()
    }
    $bmp.Save("$out\sheet_$($s + 1).png", [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}
"$($items.Count) pares, $sheets folhas"
