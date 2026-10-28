# Builds src/dsfworld/_native/meshopt.dll (x64, static CRT) from the vendored meshoptimizer
# simplifier, so that installing worldlod needs no C++ compiler. Needs the Visual Studio C++
# build tools; run it after updating native/meshoptimizer and commit the DLL.
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'meshoptimizer'
$out = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\src\dsfworld\_native'))
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force $out, $build | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools (x64) not found' }
$bat = Join-Path $build 'build.bat'
@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "$build"
cl /nologo /O2 /MT /LD /EHsc "/DMESHOPTIMIZER_API=__declspec(dllexport)" "$src\simplifier.cpp" "$src\allocator.cpp" /Fe:meshopt.dll /link /MACHINE:X64 || exit /b 1
copy /y meshopt.dll "$out\meshopt.dll" >nul
"@ | Set-Content -Encoding ascii $bat
cmd /d /c "`"$bat`""
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
Get-Item (Join-Path $out 'meshopt.dll') | Select-Object Name, Length
