# R5c: contact sheet of the `fallback` textures (shaded *_rect images from
# snap_fallback.ps1) cropped to UV 0.30-0.70 x 0.20-0.80 with a UV grid
# (thin lines every 0.01, bold every 0.05, labels every 0.1) for reading the
# tread tile edges by eye.
param(
    [string]$Project = (Resolve-Path "$PSScriptRoot\..\..").Path
)
$out = "$Project\probe\previews\r5c\fallback"
$rows = Import-Csv "$out\snapped.csv"
Add-Type -AssemblyName System.Drawing
$u0 = 0.30; $u1 = 0.70; $v0 = 0.20; $v1 = 0.80
$cw = 400; $ch = 600; $cols = 4; $head = 18
$n = $rows.Count
$sheet = New-Object System.Drawing.Bitmap ($cols * $cw), ([math]::Ceiling($n / $cols) * ($ch + $head))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font 'Arial', 9
$thin = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(110, 255, 60, 60)), 1
$bold = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(220, 255, 230, 0)), 1
for ($i = 0; $i -lt $n; $i++) {
    $img = [System.Drawing.Image]::FromFile("$out\$($rows[$i].name)_rect.bmp")
    $ox = ($i % $cols) * $cw; $oy = [math]::Floor($i / $cols) * ($ch + $head) + $head
    $src = New-Object System.Drawing.RectangleF ($img.Width * $u0), ($img.Height * $v0), ($img.Width * ($u1 - $u0)), ($img.Height * ($v1 - $v0))
    $dst = New-Object System.Drawing.RectangleF $ox, $oy, $cw, $ch
    $g.DrawImage($img, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
    for ($k = 0; $k -le 40; $k++) {
        $u = $u0 + $k * 0.01; $x = $ox + ($u - $u0) / ($u1 - $u0) * $cw
        $pen = if ($k % 5 -eq 0) { $bold } else { $thin }
        $g.DrawLine($pen, [float]$x, [float]$oy, [float]$x, [float]($oy + $ch))
        if ($k % 10 -eq 0) { $g.DrawString(('{0:0.0}' -f $u), $font, [System.Drawing.Brushes]::Yellow, [float]($x + 1), [float]($oy + 2)) }
    }
    for ($k = 0; $k -le 60; $k++) {
        $v = $v0 + $k * 0.01; $y = $oy + ($v - $v0) / ($v1 - $v0) * $ch
        $pen = if ($k % 5 -eq 0) { $bold } else { $thin }
        $g.DrawLine($pen, [float]$ox, [float]$y, [float]($ox + $cw), [float]$y)
        if ($k % 10 -eq 0) { $g.DrawString(('{0:0.0}' -f $v), $font, [System.Drawing.Brushes]::Yellow, [float]($ox + 2), [float]($y + 1)) }
    }
    $g.DrawString("$i $($rows[$i].name)", $font, [System.Drawing.Brushes]::Black, [float]$ox, [float]($oy - $head + 2))
    $img.Dispose()
}
$sheet.Save("$out\grid_sheet.png", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $sheet.Dispose()
