# Runs wearpreview on every tire colour/normal pair found in the extracted
# folders, collects the detected tread rectangles and builds a contact sheet
# of the *_rect images for a visual check. Read-only on the extracted files.
param(
    [string]$Project = (Resolve-Path "$PSScriptRoot\..\..").Path
)
$tool = "$Project\tools\wearpreview\wearpreview.exe"
$out = "$Project\probe\previews\all"
New-Item -ItemType Directory -Force $out | Out-Null
$sources = @{
    'base_vehicle' = "$Project\probe\extracted"
    'dlc_goodyear' = "$Project\probe\extracted_dlc_goodyear"
    'dlc_michelin' = "$Project\probe\extracted_dlc_michelin"
}
$rows = @()
foreach ($src in $sources.Keys) {
    $root = $sources[$src]
    Get-ChildItem $root -Recurse -Filter *.dds | Where-Object { $_.FullName -match '\\wheel\\tire\\' } | ForEach-Object {
        $f = $_
        $norm = $null
        if ($f.Name -match '^(.*)_colspec\.dds$') {
            $stem = $Matches[1]
            $cand = @("${stem}_norm.dds", (($stem -replace '^trail_', 'trai_') + '_norm.dds'))
            foreach ($c in $cand) { if (Test-Path (Join-Path $f.DirectoryName $c)) { $norm = Join-Path $f.DirectoryName $c; break } }
        } elseif ($f.Name -match '^(.*)_c((\.dlc_[a-z]+)?)\.dds$') {
            $stem = $Matches[1]; $sfx = $Matches[2]
            $c = Join-Path $f.DirectoryName "${stem}_n${sfx}.dds"
            if (Test-Path $c) { $norm = $c }
        } else { return }
        if (-not $norm) { Write-Output "NO NORMAL for $($f.FullName)"; return }
        $rel = ($f.FullName.Substring($root.Length) -replace '\\', '/')
        $name = ($f.BaseName -replace '_colspec$|_c(\.dlc_[a-z]+)?$', '')
        $prefix = "$out\${src}__$name"
        $line = & $tool $f.FullName $norm $prefix | Where-Object { $_ -like 'RECT *' }
        $parts = $line -split ' '
        $nrel = ($norm.Substring($root.Length) -replace '\\', '/')
        $rows += [pscustomobject]@{ source = $src; color = $rel; normal = $nrel; u0 = $parts[2]; v0 = $parts[3]; u1 = $parts[4]; v1 = $parts[5]; how = $parts[-3]; size = $parts[-1]; prefix = "${src}__$name" }
    }
}
$rows | Sort-Object source, color | Export-Csv "$out\rects.csv" -NoTypeInformation -Encoding utf8
Add-Type -AssemblyName System.Drawing
Get-ChildItem $out -Filter *.bmp | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $img.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose(); Remove-Item $_.FullName
}
# Contact sheet: every *_rect.png at 200 px, 6 per row, with its name.
$rects = Get-ChildItem $out -Filter *_rect.png | Sort-Object Name
$cell = 200; $label = 28; $perRow = 6
$sheetRows = [math]::Ceiling($rects.Count / $perRow)
$sheet = New-Object System.Drawing.Bitmap ($cell * $perRow), (($cell + $label) * $sheetRows)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font 'Arial', 7
for ($i = 0; $i -lt $rects.Count; $i++) {
    $x = ($i % $perRow) * $cell; $y = [math]::Floor($i / $perRow) * ($cell + $label)
    $img = [System.Drawing.Image]::FromFile($rects[$i].FullName)
    $g.DrawImage($img, $x, $y, $cell, $cell); $img.Dispose()
    $g.DrawString(($rects[$i].BaseName -replace '_rect$', ''), $font, [System.Drawing.Brushes]::Black, (New-Object System.Drawing.RectangleF $x, ($y + $cell), $cell, $label))
}
$g.Dispose()
$sheet.Save("$out\contact_sheet.png", [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
Write-Output "pairs: $($rows.Count)"
$rows | Sort-Object source, color | Format-Table prefix, u0, v0, u1, v1, how, size -AutoSize | Out-String -Width 200
