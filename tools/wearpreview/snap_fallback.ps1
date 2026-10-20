# R5c: snaps the `fallback` tread rectangles of catalog\tire_regions.csv to
# the tile edges (wearpreview ... snap), writes the proposals to
# probe\previews\r5c\fallback\snapped.csv and a contact sheet of the
# *_rect images (old rectangle in the file name list, new one drawn in green)
# for a visual check. Read-only on the catalog and the extracted textures.
param(
    [string]$Project = (Resolve-Path "$PSScriptRoot\..\..").Path
)
$tool = "$Project\tools\wearpreview\wearpreview.exe"
$out = "$Project\probe\previews\r5c\fallback"
New-Item -ItemType Directory -Force $out | Out-Null
$roots = @{
    'base_vehicle' = "$Project\probe\extracted"
    'dlc_goodyear' = "$Project\probe\extracted_dlc_goodyear"
    'dlc_michelin' = "$Project\probe\extracted_dlc_michelin"
}
$rows = @()
Import-Csv "$Project\catalog\tire_regions.csv" | Where-Object { $_.how -eq 'fallback' } | ForEach-Object {
    $r = $_
    $src, $crel = $r.color -split ':', 2
    $nrel = ($r.normal -split ':', 2)[1]
    $root = $roots[$src]
    $cpath = Join-Path $root ($crel.TrimStart('/') -replace '/', '\')
    $npath = Join-Path $root ($nrel.TrimStart('/') -replace '/', '\')
    if (-not (Test-Path $cpath) -or -not (Test-Path $npath)) { Write-Output "MISSING $cpath"; return }
    $name = [IO.Path]::GetFileNameWithoutExtension($cpath)
    $prefix = "$out\$name"
    $line = & $tool $cpath $npath $prefix $r.u0 $r.v0 $r.u1 $r.v1 snap | Where-Object { $_ -like 'RECT *' }
    $p = $line -split ' '
    $rows += [pscustomobject]@{ color = $r.color; normal = $r.normal; old = "$($r.u0) $($r.v0) $($r.u1) $($r.v1)";
        u0 = $p[2]; v0 = $p[3]; u1 = $p[4]; v1 = $p[5]; name = $name }
}
$rows | Export-Csv "$out\snapped.csv" -NoTypeInformation -Encoding utf8
$rows | Format-Table name, old, u0, v0, u1, v1 -AutoSize | Out-String -Width 200

# Contact sheet: the tread area (with a margin) of each *_rect image, 4 per row.
Add-Type -AssemblyName System.Drawing
$cell = 300
$cols = 4
$n = $rows.Count
$sheet = New-Object System.Drawing.Bitmap ($cols * $cell), ([math]::Ceiling($n / $cols) * ($cell + 20))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font 'Arial', 8
for ($i = 0; $i -lt $n; $i++) {
    $img = [System.Drawing.Image]::FromFile("$out\$($rows[$i].name)_rect.bmp")
    $src = New-Object System.Drawing.Rectangle ([int]($img.Width * 0.26)), ([int]($img.Height * 0.22)), ([int]($img.Width * 0.48)), ([int]($img.Height * 0.56))
    $dst = New-Object System.Drawing.Rectangle (($i % $cols) * $cell), ([math]::Floor($i / $cols) * ($cell + 20) + 20), $cell, $cell
    $g.DrawImage($img, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
    $g.DrawString($rows[$i].name, $font, [System.Drawing.Brushes]::Black, ($i % $cols) * $cell, [math]::Floor($i / $cols) * ($cell + 20) + 3)
    $img.Dispose()
}
$sheet.Save("$out\contact_sheet.png", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $sheet.Dispose()
