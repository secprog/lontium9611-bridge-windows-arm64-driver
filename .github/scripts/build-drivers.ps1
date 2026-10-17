# SPDX-License-Identifier: GPL-2.0-only
# Licensed under the GNU General Public License, version 2 only.
# Provided without warranty. See LICENSE for the license text.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [Parameter(Mandatory)][string]$VersionCacheDirectory
)

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$packageRoot = [IO.Path]::GetFullPath($PackageDirectory)
$buildRoot = [IO.Path]::GetFullPath($OutputDirectory)
$downloadRoot = [IO.Path]::GetFullPath($ArtifactDirectory)
foreach ($directory in @($buildRoot, $downloadRoot)) {
    if ($directory.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $directory.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build outputs must be outside the source checkout.'
    }
}

$repositoryRoot = & git -C $sourceRoot rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0 -or [IO.Path]::GetFullPath($repositoryRoot) -ne $sourceRoot) {
    throw 'Run this build from the standalone LT9611 source repository.'
}
$changes = & git -C $sourceRoot status --porcelain --untracked-files=all
if ($LASTEXITCODE -ne 0 -or $changes) {
    throw 'Commit source changes before creating a matching-source download.'
}

if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $locator -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installation)) {
        throw 'Visual Studio 2022 with ARM64 C++ tools is required.'
    }
    Import-Module (Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-host_arch=amd64 -arch=arm64'
}

$logDirectory = Join-Path $buildRoot 'logs'
$packageDestination = Join-Path $downloadRoot 'package'
$symbolDestination = Join-Path $downloadRoot 'symbols'
New-Item -ItemType Directory -Path $buildRoot, $logDirectory, $packageDestination, $symbolDestination -Force | Out-Null

[xml]$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'packages.config') -Raw
$imports = [ordered]@{
    'Microsoft.Windows.WDK.ARM64' = 'Microsoft.Windows.WDK.arm64.props'
    'Microsoft.Windows.SDK.CPP.ARM64' = 'Microsoft.Windows.SDK.cpp.arm64.props'
    'Microsoft.Windows.SDK.CPP' = 'Microsoft.Windows.SDK.cpp.props'
}
$kitVersions = @($manifest.packages.package | ForEach-Object { [string]$_.version } | Select-Object -Unique)
if ($kitVersions.Count -ne 1 -or $kitVersions[0] -notmatch '^10\.0\.26100\.\d+$' -or
    @($manifest.packages.package).Count -ne $imports.Count) {
    throw 'Use matching SDK and WDK packages from the 10.0.26100 series.'
}
$kitVersion = $kitVersions[0]
$properties = [xml]'<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003" />'
foreach ($id in $imports.Keys) {
    $entry = @($manifest.packages.package | Where-Object id -EQ $id)
    if ($entry.Count -ne 1) { throw "Missing or duplicate kit package: $id" }
    $importPath = Join-Path $packageRoot "$id.$kitVersion/build/native/$($imports[$id])"
    if (-not (Test-Path -LiteralPath $importPath -PathType Leaf)) {
        throw "Restore the SDK/WDK packages first: $importPath"
    }
    $element = $properties.CreateElement('Import', $properties.DocumentElement.NamespaceURI)
    $element.SetAttribute('Project', $importPath)
    [void]$properties.DocumentElement.AppendChild($element)
}
$propertyFile = Join-Path $buildRoot 'kits.props'
$properties.Save($propertyFile)

& (Join-Path $sourceRoot 'build-source.ps1') -OutputDirectory $buildRoot -VersionCacheDirectory $VersionCacheDirectory -WdkPropertyFile $propertyFile
$buildMetadata = Get-Content -LiteralPath (Join-Path $buildRoot 'metadata/build-version.json') -Raw | ConvertFrom-Json
$version = [string]$buildMetadata.version

foreach ($project in @('OEMBridge', 'P8CecUde', 'OEMBridgeProp')) {
    $binaryDirectory = Join-Path $buildRoot "$project/bin"
    $extension = if ($project -eq 'OEMBridgeProp') { 'dll' } else { 'sys' }
    $binary = Join-Path $binaryDirectory "$project.$extension"
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($binary))
    try {
        $reader.BaseStream.Position = 0x3c
        $offset = $reader.ReadInt32()
        $reader.BaseStream.Position = $offset + 4
        if ($reader.ReadUInt16() -ne 0xaa64) { throw "$project is not ARM64." }
    } finally {
        $reader.Dispose()
    }
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($binary).FileVersion -ne $version) {
        throw "$project version does not match the package."
    }
    Copy-Item -LiteralPath $binary -Destination $packageDestination
    Copy-Item -LiteralPath (Join-Path $binaryDirectory "$project.pdb") -Destination $symbolDestination
}

$inf = Join-Path $buildRoot 'package/OEMBridge.inf'
Copy-Item -LiteralPath $inf -Destination $packageDestination
Copy-Item -LiteralPath (Join-Path $buildRoot 'metadata/build-version.json') -Destination $downloadRoot
foreach ($name in @('LICENSE', 'LICENSES', 'NOTICE.md', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination $downloadRoot -Recurse -Force
}

$changes = & git -C $sourceRoot status --porcelain --untracked-files=all
if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Source files changed during the build.' }
& git -C $sourceRoot archive --format=zip "--output=$(Join-Path $downloadRoot 'source.zip')" HEAD
if ($LASTEXITCODE -ne 0) { throw 'Source archive creation failed.' }

if ($env:GITHUB_OUTPUT) {
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "version=$version"
}
if ($env:GITHUB_STEP_SUMMARY) {
    Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "Built Release ARM64 **$version**: OEMBridge.sys, P8CecUde.sys and OEMBridgeProp.dll. Download the artifact for the unsigned package, matching symbols, source and license notices. Generate a catalog and sign the package before installation."
}
