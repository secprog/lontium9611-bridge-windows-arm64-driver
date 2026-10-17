@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Licensed under the GNU General Public License, version 2 only.
rem Provided without warranty. See LICENSE for the license text.

setlocal
where msbuild.exe >nul 2>&1
if errorlevel 1 (
    echo Run build.cmd from an EWDK build environment.
    exit /b 1
)

set "LT9611_OUTPUT=%TEMP%\lt9611-build"
if not "%~1"=="" set "LT9611_OUTPUT=%~f1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-source.ps1" -OutputDirectory "%LT9611_OUTPUT%"
exit /b %errorlevel%
