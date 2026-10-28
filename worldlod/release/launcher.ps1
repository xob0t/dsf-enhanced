<#
.SYNOPSIS
The PowerShell half of install.cmd: makes sure uv is installed and the game folder is known, then
runs `uv run worldlod install` (or uninstall) with install.cmd's arguments.

.DESCRIPTION
The release ZIP holds install.cmd, app\ (the worldlod package, this script and uv.lock) and
plugin\ (DSF Enhanced from the same build, installed into the game if it lacks it).
- uv (https://docs.astral.sh/uv/) runs worldlod; it brings its own Python 3.13 and the locked
  dependencies into app\.venv. When uv is missing it is installed with Astral's official
  installer, pinned to $UvVersion (into %USERPROFILE%\.local\bin).
- The game folder: --game DIR; else a folder around this one holding Driver.exe (the ZIP unpacked
  into the game folder); else DSF_GAME_DIR (this window's, else your user's); else it asks once
  and saves the answer as your user's DSF_GAME_DIR.

.EXAMPLE
install.cmd
install.cmd --uninstall
install.cmd --no-buildings
install.cmd --help
#>
$ErrorActionPreference = 'Stop'
$UvVersion = '0.13.5'
$App = $PSScriptRoot
$Root = Split-Path -Parent $App
$Forward = @($args)

function Find-Uv {
    $cmd = Get-Command uv -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($dir in @((Join-Path $env:USERPROFILE '.local\bin'), (Join-Path $env:USERPROFILE '.cargo\bin'))) {
        $exe = Join-Path $dir 'uv.exe'
        if (Test-Path -LiteralPath $exe) { return $exe }
    }
    return $null
}

function Test-Game([string]$Path) {
    return ($Path -and (Test-Path -LiteralPath (Join-Path $Path 'Driver.exe')))
}

$uv = Find-Uv
if (-not $uv) {
    Write-Host "uv (it runs worldlod and brings its own Python) is not installed."
    Write-Host "Installing uv $UvVersion with the official installer from https://astral.sh/uv ..."
    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "irm https://astral.sh/uv/$UvVersion/install.ps1 | iex"
    $ErrorActionPreference = 'Stop'
    $uv = Find-Uv
    if (-not $uv) {
        throw 'uv could not be installed. Install it yourself (winget install astral-sh.uv), open a new window and run install.cmd again.'
    }
    Write-Host "uv is installed ($uv). (The PATH note above is for new terminals; install.cmd goes on by itself.)"
}

$command = 'install'
if ($Forward -contains '--uninstall') {
    $command = 'uninstall'
    $Forward = @($Forward | Where-Object { $_ -ne '--uninstall' })
}
$help = $Forward | Where-Object { $_ -in @('-h', '--help') }
if (-not ($Forward -contains '--game') -and -not $help) {
    $game = @($Root, (Split-Path -Parent $Root)) | Where-Object { Test-Game $_ } | Select-Object -First 1
    if (-not $game) { $game = $env:DSF_GAME_DIR }
    if (-not (Test-Game $game)) { $game = [Environment]::GetEnvironmentVariable('DSF_GAME_DIR', 'User') }
    if (-not (Test-Game $game)) {
        if ($game) { Write-Host "DSF_GAME_DIR is $game, but there is no Driver.exe there." }
        $asked = $null
        for ($try = 0; $try -lt 5 -and -not (Test-Game $asked); $try++) {
            $asked = (Read-Host 'Driver: San Francisco folder (the one with Driver.exe)').Trim().Trim('"')
            if (-not (Test-Game $asked)) { Write-Host "No Driver.exe in '$asked'." }
        }
        if (-not (Test-Game $asked)) { throw 'No game folder given.' }
        $game = (Resolve-Path -LiteralPath $asked).Path
        [Environment]::SetEnvironmentVariable('DSF_GAME_DIR', $game, 'User')
        Write-Host "Saved as your DSF_GAME_DIR: $game"
    }
    $Forward = @('--game', $game) + $Forward
}
if ($command -eq 'install') { $Forward = @('--plugin', (Join-Path $Root 'plugin')) + $Forward }

# uv reports progress on stderr; Windows PowerShell must not treat that as an error.
$env:WORLDLOD_LAUNCHER = 'install.cmd'   # the summary names install.cmd --uninstall
$env:WORLDLOD_WORK = Join-Path $Root 'work'   # build files beside install.cmd (deleted after install)
$ErrorActionPreference = 'Continue'
& $uv run --project $App --frozen --no-dev --quiet worldlod $command @Forward
exit $LASTEXITCODE
