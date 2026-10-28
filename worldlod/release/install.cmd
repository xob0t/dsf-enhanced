@echo off
rem Builds the high-detail upper Shift map from your copy of Driver: San Francisco and installs it,
rem with DSF Enhanced if the game doesn't have it yet. See README.md.
rem   install.cmd                  build and install
rem   install.cmd --uninstall      put the game back as it was
rem   install.cmd --no-buildings   leave building detail out (also --no-trees, --no-road-text)
rem   install.cmd --help           every option
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0app\launcher.ps1" %*
set WORLDLOD_EXIT=%ERRORLEVEL%
rem Opened by a double click (no arguments, its own window: cmd /c ""<folder>\install.cmd" "): keep the
rem window open to read the summary. The command line is only read through delayed expansion: a folder
rem name with & or ) in it would break a plain %cmdcmdline%.
if not "%~1"=="" exit /b %WORLDLOD_EXIT%
setlocal EnableDelayedExpansion
set "WORLDLOD_LINE=!cmdcmdline!"
if /i not "!WORLDLOD_LINE:%~nx0=!"=="!WORLDLOD_LINE!" pause
exit /b %WORLDLOD_EXIT%
