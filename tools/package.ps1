# Builds the release zip: the DLL (catalog, font and sounds built in), the
# settings file to put next to it, the read-me, LICENSE (MIT) and
# THIRD_PARTY_LICENSES (notices of what is built in, the font's OFL included).
# Usage: tools\package.ps1 -Version 0.3.2-beta
# Writes dist\ets2-tire-wear.zip (a fixed name, so the README's
# "releases/latest/download/ets2-tire-wear.zip" link always works) and a copy
# named with the version.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Project = (Resolve-Path "$PSScriptRoot\..").Path
)
$ErrorActionPreference = 'Stop'
$dll = "$Project\build\out\ets2_tire_wear.dll"
if (-not (Test-Path $dll)) { throw "build the Release DLL first ($dll)" }
$stage = "$Project\dist\ets2-tire-wear-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll, "$Project\package\LEIA-ME.txt" $stage
Copy-Item "$Project\LICENSE" "$stage\LICENSE.txt"
Copy-Item "$Project\THIRD_PARTY_LICENSES.txt" "$stage\THIRD_PARTY_LICENSES.txt"

# The INI is the very text the plugin writes when it has none (the string in
# present_probe.cpp's LoadConfig), so the two never drift apart.
$src = Get-Content "$Project\src\present_probe.cpp" -Raw
$m = [regex]::Match($src, 'fputs\((?<body>(?:\s*"(?:[^"\\]|\\.)*")+)\s*,\s*f\);')
if (-not $m.Success) { throw 'INI text not found in present_probe.cpp' }
$text = ([regex]::Matches($m.Groups['body'].Value, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value }) -join ''
$text = $text.Replace('\r', "`r").Replace('\n', "`n").Replace('\"', '"').Replace('\\', '\')
if ($text -notmatch '\[blowout\]' -or $text -notmatch 'wear_override') { throw 'INI text looks incomplete' }
$utf8 = New-Object Text.UTF8Encoding $false
[IO.File]::WriteAllText("$stage\ets2_tire_wear.ini", $text, $utf8)

# testes\: ready-made copies of that INI with the test keys set; one is
# copied over plugins\ets2_tire_wear.ini to try a feature right away.
function Preset([string]$file, [string]$title, [hashtable]$set) {
    $t = $text
    foreach ($k in $set.Keys) {
        $t = [regex]::Replace($t, "(?m)^$k = .*$", "$k = $($set[$k])`r")
        $t = $t.Replace("`r`r", "`r")
    }
    if ($set.Count -and $t -eq $text) { throw "preset $file changed nothing" }
    [IO.File]::WriteAllText("$stage\testes\$file", "; $title`r`n" + $t, $utf8)
}
New-Item -ItemType Directory -Force "$stage\testes" | Out-Null
Preset '1-uso-normal.ini' 'USO NORMAL / NORMAL USE (igual ao padrao / same as the default)' @{}
Preset '2-ver-desgaste.ini' 'VER O DESGASTE: todos os seus pneus a 86% / SEE THE WEAR: all your tires at 86%' @{ wear_override = '0.86' }
Preset '3-estouro-caminhao.ini' 'ESTOURO NO CAMINHAO: pneu dianteiro esquerdo, 20 s depois de andar / TRUCK BLOWOUT: front left tire, 20 s after driving off' @{ test_after_s = '20'; test_unit = 'truck'; test_wheel = '0' }
Preset '4-estouro-carreta.ini' 'ESTOURO NA CARRETA: 1o eixo, esquerda, 20 s depois de andar (com carreta engatada) / TRAILER BLOWOUT: first axle, left, 20 s after driving off (trailer attached)' @{ test_after_s = '20'; test_unit = 'trailer'; test_wheel = '0' }

$zip = "$Project\dist\ets2-tire-wear.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Copy-Item $zip "$Project\dist\ets2-tire-wear-$Version.zip" -Force
Get-ChildItem $stage -File -Recurse | ForEach-Object { '{0,-34} {1,8} {2}' -f $_.FullName.Substring($stage.Length + 1), $_.Length, (Get-FileHash $_.FullName).Hash.Substring(0, 16) }
"zip: $zip  sha256 $((Get-FileHash $zip).Hash)"
