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
$commands = @(
  "cl /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:`"$build\\`" /Fe:`"$asi`" `"$root\src\dsf_enhanced.cpp`" user32.lib d3d9.lib dxguid.lib /link /MACHINE:X86 /MAP:`"$build\dsf_enhanced.map`"",
  "cl /nologo /std:c++17 /EHsc /O2 /Fo:`"$build\\`" /Fe:`"$test`" `"$root\tests\cinematic_fit_test.cpp`"",
  "`"$test`""
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

$packageFiles = @("winmm.dll", "dsf_enhanced.asi", "dsf_enhanced.ini", "README.md", "LICENSE") |
  ForEach-Object { Join-Path $dist $_ }
Compress-Archive -LiteralPath $packageFiles -DestinationPath (Join-Path $dist "DSF-Enhanced.zip")

# worldlod: install.cmd, the pipeline and this build of the plugin (worldlod\release\package.ps1)
& (Join-Path $root "worldlod\release\package.ps1") -Dist $dist

Write-Host "Built release package in $dist"
