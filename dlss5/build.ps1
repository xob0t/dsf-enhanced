param(
  # Skip the 32-bit protocol test client (needs the x86 toolchain).
  [switch]$SkipClient
)

# Builds the x64 DLSS 5 helper, its offscreen test and the x86 protocol test client with
# the VS 2022 toolchain into dlss5\build. Fetches the public DLSS SDK headers and static
# NGX loader on first use (see fetch-sdk.ps1); nothing from NVIDIA is committed.

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$build = Join-Path $root "build"
$sdk = Join-Path $root "external\dlss-sdk"
if (-not (Test-Path -LiteralPath (Join-Path $sdk "lib\nvsdk_ngx_s.lib"))) {
  & (Join-Path $root "fetch-sdk.ps1")
}
$x64 = Join-Path $build "x64"
$x86 = Join-Path $build "x86"
foreach ($dir in @($x64, $x86)) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
  throw "vswhere.exe is missing; install the Visual Studio 2022 C++ build tools."
}
$installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installationPath) {
  throw "Could not find a Visual Studio installation with the C++ tools."
}
$vcvarsall = Join-Path $installationPath "VC\Auxiliary\Build\vcvarsall.bat"

$common = @("/nologo", "/std:c++17", "/EHsc", "/O2", "/MT", "/W3", "/DUNICODE", "/D_UNICODE", "/DNOMINMAX",
  "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS", "/I`"$root\include`"")
$ngx = @("/I`"$sdk\include`"")
$shared = @("src\nr_d3d12.cpp", "src\ngx_trust.cpp", "src\d3d12_util.cpp") | ForEach-Object { "`"$root\$_`"" }
$ngxLibs = @("`"$sdk\lib\nvsdk_ngx_s.lib`"", "d3d12.lib", "dxgi.lib", "advapi32.lib", "user32.lib", "ole32.lib")

# One cl invocation per target, its arguments in a response file so paths need no shell quoting.
# vcvarsall looks for vswhere on PATH.
$installerDir = Split-Path -Parent $vswhere
function Invoke-Cl([string]$arch, [string]$name, [string[]]$arguments) {
  $outDir = Join-Path $build $arch
  $rsp = Join-Path $outDir "$name.rsp"
  $all = $common + $arguments + @("/Fo`"$outDir\\`"", "/Fe`"$outDir\$name.exe`"")
  Set-Content -LiteralPath $rsp -Value ($all -join "`r`n") -Encoding ascii
  $machine = if ($arch -eq "x64") { "X64" } else { "X86" }
  cmd /d /c "set `"PATH=%PATH%;$installerDir`" && call `"$vcvarsall`" $arch >nul && cl @`"$rsp`" /link /MACHINE:$machine"
  if ($LASTEXITCODE -ne 0) {
    throw "Building $name ($arch) failed with exit code $LASTEXITCODE"
  }
}

$offscreen = @("`"$root\tests\offscreen_test.cpp`"", "`"$root\tests\synthetic_scene.cpp`"", "`"$root\src\image_io.cpp`"")
Invoke-Cl "x64" "dsf-dlss5-offscreen" ($ngx + $offscreen + $shared + $ngxLibs)
Invoke-Cl "x64" "dsf-dlss5-helper" ($ngx + @("`"$root\src\helper_main.cpp`"") + $shared + $ngxLibs)
if (-not $SkipClient) {
  $client = @("`"$root\tests\client_test.cpp`"", "`"$root\tests\synthetic_scene.cpp`"", "`"$root\src\image_io.cpp`"")
  Invoke-Cl "x86" "dsf-dlss5-client-test" ($client + @("d3d11.lib", "dxgi.lib", "user32.lib", "ole32.lib"))
}

Write-Host "Built the DLSS 5 helper and tests in $build"
