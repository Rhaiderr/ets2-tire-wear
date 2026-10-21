# Builds the release zip: the DLL (catalog, font and sounds built in), the
# settings file to put next to it, the read-me and the licences.
# Usage: tools\package.ps1 -Version 0.3.0-beta
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
Copy-Item "$Project\third_party\rubik\OFL.txt" "$stage\OFL-Rubik.txt"

# The INI is the very text the plugin writes when it has none (the string in
# present_probe.cpp's LoadConfig), so the two never drift apart.
$src = Get-Content "$Project\src\present_probe.cpp" -Raw
$m = [regex]::Match($src, 'fputs\((?<body>(?:\s*"(?:[^"\\]|\\.)*")+)\s*,\s*f\);')
if (-not $m.Success) { throw 'INI text not found in present_probe.cpp' }
$text = ([regex]::Matches($m.Groups['body'].Value, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value }) -join ''
$text = $text.Replace('\r', "`r").Replace('\n', "`n").Replace('\"', '"').Replace('\\', '\')
if ($text -notmatch '\[blowout\]' -or $text -notmatch 'ffb_device') { throw 'INI text looks incomplete' }
[IO.File]::WriteAllText("$stage\ets2_tire_wear.ini", $text, (New-Object Text.UTF8Encoding $false))

$zip = "$Project\dist\ets2-tire-wear.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Copy-Item $zip "$Project\dist\ets2-tire-wear-$Version.zip" -Force
Get-ChildItem $stage -File | ForEach-Object { '{0,-24} {1,8} {2}' -f $_.Name, $_.Length, (Get-FileHash $_.FullName).Hash.Substring(0, 16) }
"zip: $zip  sha256 $((Get-FileHash $zip).Hash)"
