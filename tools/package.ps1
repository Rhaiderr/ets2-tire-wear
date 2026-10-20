# Builds the release zip from build\out\ets2_tire_wear.dll, catalog\ and
# package\TireWear\ets2_tire_wear.ini. The zip mirrors the two places the
# files go:
#   plugins\ets2_tire_wear.dll   -> <game>\bin\win_x64\plugins\
#   TireWear\*                   -> Documents\Euro Truck Simulator 2\TireWear\
# Usage: tools\package.ps1 -Version 0.1.0-alpha
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Project = (Resolve-Path "$PSScriptRoot\..").Path
)
$ErrorActionPreference = 'Stop'
$dll = "$Project\build\out\ets2_tire_wear.dll"
if (-not (Test-Path $dll)) { throw "build the Release DLL first ($dll)" }
$name = "ets2-tire-wear-$Version"
$stage = "$Project\dist\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\plugins", "$stage\TireWear" | Out-Null
Copy-Item $dll "$stage\plugins\"
Copy-Item "$Project\catalog\tire_textures.csv", "$Project\catalog\tire_regions.csv" "$stage\TireWear\"
Copy-Item "$Project\package\TireWear\ets2_tire_wear.ini" "$stage\TireWear\"
Copy-Item "$Project\package\LEIA-ME.txt" "$stage\"
Copy-Item "$Project\LICENSE" "$stage\" -ErrorAction SilentlyContinue
Copy-Item "$Project\third_party\minhook\LICENSE.txt" "$stage\LICENSE-minhook.txt"
Copy-Item "$Project\third_party\imgui\LICENSE.txt" "$stage\LICENSE-imgui.txt"
$zip = "$Project\dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    '{0,-40} {1}' -f $_.FullName.Substring($stage.Length + 1), (Get-FileHash $_.FullName).Hash.Substring(0, 16)
}
"zip: $zip  sha256 $((Get-FileHash $zip).Hash)"
