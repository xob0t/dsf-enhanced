param(
  [string]$Tag = "v310.9.16",
  [string]$Destination = (Join-Path $PSScriptRoot "external\dlss-sdk")
)

# Downloads the parts of NVIDIA's public DLSS SDK (https://github.com/NVIDIA/DLSS) that the
# helper links against: the NGX headers and the static NGX loader library. They are NVIDIA
# software under the NVIDIA RTX SDKs license, so they are fetched at build time into a
# gitignored folder and never committed. No NVIDIA runtime DLL is downloaded here.

$ErrorActionPreference = "Stop"

$headers = @(
  "nvsdk_ngx.h",
  "nvsdk_ngx_defs.h",
  "nvsdk_ngx_params.h"
)
$libs = @("nvsdk_ngx_s.lib")

$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Tag"
$include = Join-Path $Destination "include"
$lib = Join-Path $Destination "lib"
foreach ($dir in @($include, $lib)) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

foreach ($name in $headers) {
  Invoke-WebRequest -UseBasicParsing -Uri "$base/include/$name" -OutFile (Join-Path $include $name)
}
foreach ($name in $libs) {
  Invoke-WebRequest -UseBasicParsing -Uri "$base/lib/Windows_x86_64/x64/$name" -OutFile (Join-Path $lib $name)
}
Invoke-WebRequest -UseBasicParsing -Uri "$base/LICENSE.txt" -OutFile (Join-Path $Destination "LICENSE.txt")
Set-Content -LiteralPath (Join-Path $Destination "TAG") -Value $Tag -Encoding ascii

Write-Host "Fetched DLSS SDK $Tag into $Destination"
