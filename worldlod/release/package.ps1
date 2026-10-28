<#
.SYNOPSIS
Assembles dist\worldlod.zip: install.cmd, app\ (the worldlod package with uv.lock and the
launcher) and plugin\ (the DSF Enhanced just built into -Dist). build.ps1 calls it last.
#>
param([Parameter(Mandatory)] [string]$Dist)
$ErrorActionPreference = 'Stop'
$package = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $Dist 'worldlod'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$app = Join-Path $stage 'app'
$plugin = Join-Path $stage 'plugin'
New-Item -ItemType Directory -Force $app, $plugin | Out-Null

foreach ($f in 'pyproject.toml', 'uv.lock', 'README.md') { Copy-Item -LiteralPath (Join-Path $package $f) $app }
Copy-Item -LiteralPath (Join-Path $package 'src') -Destination $app -Recurse
Get-ChildItem -LiteralPath (Join-Path $app 'src') -Recurse -Directory -Filter '__pycache__' | Remove-Item -Recurse -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'launcher.ps1') $app
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install.cmd') $stage
Copy-Item -LiteralPath (Join-Path $package 'README.md') $stage
foreach ($f in 'dsf_enhanced.asi', 'dsf_enhanced.ini', 'winmm.dll', 'LICENSE') { Copy-Item -LiteralPath (Join-Path $Dist $f) $plugin }
# third-party notices: meshoptimizer (app\src\dsfworld\_native\meshopt.dll) and Ultimate ASI Loader (plugin\winmm.dll)
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $package 'native\meshoptimizer\LICENSE.md') (Join-Path $licenses 'meshoptimizer.txt')
Copy-Item -LiteralPath (Join-Path $package '..\third_party\Ultimate-ASI-Loader.LICENSE.txt') (Join-Path $licenses 'Ultimate-ASI-Loader.txt')

$zip = Join-Path $Dist 'worldlod.zip'
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $stage install.cmd README.md app plugin licenses  # standard / paths
if ($LASTEXITCODE) { throw "tar failed ($LASTEXITCODE)" }
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host "Built $zip"
