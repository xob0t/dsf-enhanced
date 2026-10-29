param(
  # Leave out the DLSS 5 helper (it needs the x64 toolchain and downloads NVIDIA's DLSS SDK
  # headers and static loader, pinned by SHA-256, from github.com/NVIDIA/DLSS).
  [switch]$SkipDlss5
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$build = Join-Path $root "build"
$dist = Join-Path $root "dist"
foreach ($dir in @($build, $dist)) {
  if (Test-Path -LiteralPath $dir) {
    Remove-Item -LiteralPath $dir -Recurse -Force
  }
  New-Item -ItemType Directory -Path $dir | Out-Null
}

$asi = Join-Path $build "dsf_enhanced.asi"
$test = Join-Path $build "cinematic_fit_test.exe"
$jitterTest = Join-Path $build "vs_jitter_test.exe"
$commands = @(
  "cl /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:`"$build\\`" /Fe:`"$asi`" `"$root\src\dsf_enhanced.cpp`" user32.lib d3d9.lib dxguid.lib /link /MACHINE:X86 /MAP:`"$build\dsf_enhanced.map`"",
  "cl /nologo /std:c++17 /EHsc /O2 /Fo:`"$build\\`" /Fe:`"$test`" `"$root\tests\cinematic_fit_test.cpp`"",
  "`"$test`"",
  # DLAA jitter's vertex shader patch: the self-test (d3dcompiler_47). The file says how to run it
  # over the game's shaders and on the GPU.
  "cl /nologo /std:c++17 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS /Fo:`"$build\\`" /Fe:`"$jitterTest`" `"$root\tests\vs_jitter_test.cpp`" d3d9.lib user32.lib",
  "`"$jitterTest`""
)
# Optional: checks src/vk_min.h against the Khronos Vulkan headers (a Vulkan-Headers include folder).
if ($env:VULKAN_HEADERS_INCLUDE) {
  $vkCheck = Join-Path $build "vk_min_check.exe"
  $commands += "cl /nologo /std:c++17 /EHsc /I`"$env:VULKAN_HEADERS_INCLUDE`" /Fo:`"$build\\`" /Fe:`"$vkCheck`" `"$root\tests\vk_min_check.cpp`""
  $commands += "`"$vkCheck`""
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Could not find cl.exe in PATH and vswhere.exe is missing."
  }

  # vcvars32.bat calls vswhere itself and needs it on PATH.
  $env:PATH = (Split-Path -Parent $vswhere) + ";" + $env:PATH
  $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $installationPath) {
    throw "Could not find a Visual Studio installation with x86 C++ tools."
  }

  $vcvars = Join-Path $installationPath "VC\Auxiliary\Build\vcvars32.bat"
  if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "Could not find vcvars32.bat at $vcvars"
  }

  $commands = @("call `"$vcvars`" >nul") + $commands
}

cmd /d /c ($commands -join " && ")
if ($LASTEXITCODE -ne 0) {
  throw "Build or test failed with exit code $LASTEXITCODE"
}

Copy-Item -LiteralPath $asi -Destination $dist
Copy-Item -LiteralPath (Join-Path $root "src\dsf_enhanced.ini") -Destination $dist
Copy-Item -LiteralPath (Join-Path $root "third_party\winmm.dll") -Destination $dist
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $dist
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $dist
$packageEntries = @("winmm.dll", "dsf_enhanced.asi", "dsf_enhanced.ini", "README.md", "LICENSE")

# DLSS 5 (experimental): dist\dlss5 holds the helper, NVIDIA's license for the static NGX loader
# linked into it (fetched with the SDK, never committed) and a readme. No NVIDIA DLL and no DXVK.
if (-not $SkipDlss5) {
  & (Join-Path $root "dlss5\build.ps1")
  $dlss5Dist = Join-Path $dist "dlss5"
  New-Item -ItemType Directory -Path $dlss5Dist | Out-Null
  Copy-Item -LiteralPath (Join-Path $root "dlss5\build\x64\dsf-dlss5-helper.exe") -Destination $dlss5Dist
  Copy-Item -LiteralPath (Join-Path $root "dlss5\external\dlss-sdk\LICENSE.txt") -Destination (Join-Path $dlss5Dist "NVIDIA-RTX-SDKs-LICENSE.txt")
  Copy-Item -LiteralPath (Join-Path $root "src\dlss5-folder-README.txt") -Destination (Join-Path $dlss5Dist "README.txt")
  $packageEntries += "dlss5"
}

# tar writes standard "/" paths, which every unzip tool reads (Compress-Archive on PowerShell 5.1
# writes "\").
& "$env:SystemRoot\System32\tar.exe" -a -c -f (Join-Path $dist "DSF-Enhanced.zip") -C $dist @packageEntries
if ($LASTEXITCODE -ne 0) {
  throw "Packing DSF-Enhanced.zip failed with exit code $LASTEXITCODE"
}

# worldlod: install.cmd, the pipeline and this build of the plugin (worldlod\release\package.ps1)
& (Join-Path $root "worldlod\release\package.ps1") -Dist $dist

Write-Host "Built release package in $dist"
