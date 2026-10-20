# R5c: per-mip check of every colour/normal pair of catalog\<game>\tire_regions.csv
# (wearpreview ... mips: each mip transformed on its own, as in the game).
# Sheets of 11 pairs, one row each: mips 0-3 at 0% then at 100%.
# Read-only on the catalog and the extracted textures.
param(
    [ValidateSet('ets2', 'ats')][string]$Game = 'ets2',
    [string]$Project = (Resolve-Path "$PSScriptRoot\..\..").Path
)
$tool = "$Project\tools\wearpreview\wearpreview.exe"
$out = "$Project\probe\previews\r5d\$Game\mips"
New-Item -ItemType Directory -Force $out | Out-Null
$roots = . "$PSScriptRoot\roots.ps1" $Project $Game
$items = @()
Import-Csv "$Project\catalog\$Game\tire_regions.csv" | ForEach-Object {
    $src, $crel = $_.color -split ':', 2
    $nrel = ($_.normal -split ':', 2)[1]
    $c = Join-Path $roots[$src] ($crel.TrimStart('/') -replace '/', '\')
    $n = Join-Path $roots[$src] ($nrel.TrimStart('/') -replace '/', '\')
    if (-not (Test-Path $c) -or -not (Test-Path $n)) { Write-Output "MISSING $c"; return }
    $name = [IO.Path]::GetFileNameWithoutExtension($c) -replace '_colspec$|_c(\.dlc_[a-z]+)?$', ''
    & $tool $c $n "$out\$name" $_.u0 $_.v0 $_.u1 $_.v1 mips | Out-Null
    $items += $name
}
Add-Type -AssemblyName System.Drawing
$cellW = 120; $cellH = 170; $gap = 4; $head = 14; $perSheet = 11
$font = New-Object System.Drawing.Font 'Arial', 8
$sheets = [math]::Ceiling($items.Count / $perSheet)
for ($s = 0; $s -lt $sheets; $s++) {
    $bmp = New-Object System.Drawing.Bitmap (8 * ($cellW + $gap) + 20), ($perSheet * ($cellH + $head + $gap))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $g.InterpolationMode = 'NearestNeighbor'
    for ($k = 0; $k -lt $perSheet; $k++) {
        $i = $s * $perSheet + $k
        if ($i -ge $items.Count) { break }
        $img = [System.Drawing.Image]::FromFile("$out\$($items[$i])_mips.bmp")
        # _mips.bmp: 4 cells of width tw (+8 gap), 2 rows of height th (+8 gap)
        $tw = ($img.Width - 24) / 4; $th = ($img.Height - 8) / 2
        $oy = $k * ($cellH + $head + $gap)
        $g.DrawString($items[$i], $font, [System.Drawing.Brushes]::Black, [float]0, [float]$oy)
        for ($row = 0; $row -lt 2; $row++) {
            for ($m = 0; $m -lt 4; $m++) {
                $src = New-Object System.Drawing.Rectangle ([int]($m * ($tw + 8))), ([int]($row * ($th + 8))), ([int]$tw), ([int]([math]::Min($th, $tw * $cellH / $cellW)))
                $ox = ($row * 4 + $m) * ($cellW + $gap) + $row * 20
                $dst = New-Object System.Drawing.Rectangle $ox, ($oy + $head), $cellW, $cellH
                $g.DrawImage($img, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
            }
        }
        $img.Dispose()
    }
    $bmp.Save("$out\mips_sheet_$($s + 1).png", [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}
"$($items.Count) pares, $sheets folhas"
