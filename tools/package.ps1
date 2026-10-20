# Builds the release zip: the DLL (catalog built in), the read-me and the
# licence. Usage: tools\package.ps1 -Version 0.2.0-alpha
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
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll, "$Project\package\LEIA-ME.txt" $stage
Copy-Item "$Project\LICENSE" "$stage\LICENSE.txt"
$zip = "$Project\dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-ChildItem $stage -File | ForEach-Object { '{0,-24} {1}' -f $_.Name, (Get-FileHash $_.FullName).Hash.Substring(0, 16) }
"zip: $zip  sha256 $((Get-FileHash $zip).Hash)"
