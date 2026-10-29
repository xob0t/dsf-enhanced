param(
  [string]$Destination = (Join-Path $PSScriptRoot "external\dlss-sdk")
)

# Downloads the parts of NVIDIA's public DLSS SDK (https://github.com/NVIDIA/DLSS) that the
# helper links against: the NGX headers and the static NGX loader library. They are NVIDIA
# software under the NVIDIA RTX SDKs license, so they are fetched at build time into a
# gitignored folder and never committed. No NVIDIA runtime DLL is downloaded here.
#
# The files come from a fixed commit (the "DLSS 310.9.16 SDK" release) and each must match
# its SHA-256 below; nvsdk_ngx_s.lib is linked statically into the helper. A file that is
# missing or does not match is downloaded again, and a download that still does not match
# stops the build.

$ErrorActionPreference = "Stop"

$commit = "9981c6350dc58c0ce21f024fd65e31e851c31689"
$files = [ordered]@{
  "include/nvsdk_ngx.h"                     = @("include\nvsdk_ngx.h", "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848")
  "include/nvsdk_ngx_defs.h"                = @("include\nvsdk_ngx_defs.h", "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e")
  "include/nvsdk_ngx_params.h"              = @("include\nvsdk_ngx_params.h", "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc")
  "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" = @("lib\nvsdk_ngx_s.lib", "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765")
  "LICENSE.txt"                             = @("LICENSE.txt", "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0")
}

function Test-Hash([string]$path, [string]$expected) {
  (Test-Path -LiteralPath $path -PathType Leaf) -and
    ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expected)
}

$fetched = 0
foreach ($source in $files.Keys) {
  $target = Join-Path $Destination $files[$source][0]
  $expected = $files[$source][1]
  if (Test-Hash $target $expected) {
    continue
  }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
  Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/NVIDIA/DLSS/$commit/$source" -OutFile $target
  if (-not (Test-Hash $target $expected)) {
    Remove-Item -LiteralPath $target -Force
    throw "SHA-256 of $source from NVIDIA/DLSS@$commit does not match the pinned value; refusing to use it."
  }
  $fetched++
}
Set-Content -LiteralPath (Join-Path $Destination "COMMIT") -Value $commit -Encoding ascii

if ($fetched -ne 0) {
  Write-Host "Fetched $fetched DLSS SDK file(s) from NVIDIA/DLSS@$commit into $Destination"
}
