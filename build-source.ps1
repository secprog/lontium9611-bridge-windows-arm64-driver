# SPDX-License-Identifier: GPL-2.0-only
# Licensed under the GNU General Public License, version 2 only.
# Provided without warranty. See LICENSE for the license text.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$VersionCacheDirectory = '',
    [version]$Version,
    [string]$WdkPropertyFile = ''
)

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$explicitVersion = $PSBoundParameters.ContainsKey('Version')

function Assert-Version([version]$Value) {
    if ($null -eq $Value -or $Value.Build -lt 0 -or $Value.Revision -lt 0 -or
        $Value.Major -gt 65535 -or $Value.Minor -gt 65535 -or
        $Value.Build -gt 65535 -or $Value.Revision -gt 65535) {
        throw 'Version must contain four components between 0 and 65535.'
    }
}

function Get-ExternalPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $sourceRoot.StartsWith($full.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build outputs and version cache must be outside the source folder and its parents.'
    }
    return $full
}

function Get-SourceFingerprint {
    $entries = @()
    foreach ($directory in @('OEMBridge', 'P8CecUde', 'OEMBridgeProp', 'LICENSES', '.github')) {
        $entries += @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot $directory) -File -Recurse | Where-Object {
            $_.FullName.Substring($sourceRoot.Length + 1) -notmatch '(^|[\\/])(ARM64|x64|obj|out|\.vs)([\\/]|$)' -and
            $_.Extension -in @('.c', '.h', '.rc', '.inf', '.vcxproj', '.def', '.ps1', '.yml', '.config', '.txt')
        })
    }
    $entries += @(Get-ChildItem -LiteralPath $sourceRoot -File | Where-Object Name -NE '.git')
    $files = [Collections.Generic.Dictionary[string, IO.FileInfo]]::new([StringComparer]::Ordinal)
    foreach ($entry in $entries) {
        $name = $entry.FullName.Substring($sourceRoot.TrimEnd('\').Length + 1).Replace('\', '/')
        $files[$name] = $entry
    }
    $names = [string[]]@($files.Keys)
    [Array]::Sort($names, [StringComparer]::Ordinal)
    $manifest = [Text.StringBuilder]::new()
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($name in $names) {
            $text = [IO.File]::ReadAllText($files[$name].FullName).Replace("`r`n", "`n")
            $hash = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-', '')
            [void]$manifest.Append($name).Append([char]0).Append($hash).Append("`n")
        }
        return ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($manifest.ToString())))).Replace('-', '').ToLowerInvariant()
    } finally {
        $hasher.Dispose()
    }
}

function Get-NextVersion([version]$Previous) {
    $parts = @($Previous.Major, $Previous.Minor, $Previous.Build, $Previous.Revision)
    for ($index = 3; $index -ge 0; $index--) {
        if ($parts[$index] -lt 65535) {
            $parts[$index]++
            return [version]($parts -join '.')
        }
        $parts[$index] = 0
    }
    throw 'Driver version range exhausted.'
}

if ($explicitVersion) { Assert-Version $Version }
if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
    throw 'Run this script from a configured WDK/EWDK build environment.'
}
$buildRoot = Get-ExternalPath $OutputDirectory
if ([string]::IsNullOrWhiteSpace($VersionCacheDirectory)) {
    $VersionCacheDirectory = Join-Path $env:LOCALAPPDATA 'LT9611/SourceVersions'
}
$cacheRoot = Get-ExternalPath $VersionCacheDirectory
$metadataRoot = Join-Path $buildRoot 'metadata'
$packageRoot = Join-Path $buildRoot 'package'
$logRoot = Join-Path $buildRoot 'logs'
New-Item -ItemType Directory -Path $metadataRoot, $packageRoot, $logRoot, $cacheRoot -Force | Out-Null
$statePath = Join-Path $cacheRoot 'source-versions.json'
$hasher = [Security.Cryptography.SHA256]::Create()
try {
    $mutexKey = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($statePath.ToLowerInvariant())))).Replace('-', '')
} finally {
    $hasher.Dispose()
}
$mutex = [Threading.Mutex]::new($false, ('Local\LT9611SourceVersion-' + $mutexKey))
$lockHeld = $false
try {
    try { $lockHeld = $mutex.WaitOne() }
    catch [Threading.AbandonedMutexException] { $lockHeld = $true }
    $fingerprint = Get-SourceFingerprint
    $versions = @{}
    $highest = [version]'1.0.0.0'
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        if ($state.schema -ne 1 -or $null -eq $state.versions) { throw 'Invalid source-version cache.' }
        foreach ($entry in $state.versions.PSObject.Properties) {
            if ($entry.Name -notmatch '^[0-9a-f]{64}$') { throw 'Invalid cached source fingerprint.' }
            $cached = [version]$entry.Value
            Assert-Version $cached
            $versions[$entry.Name] = $cached.ToString(4)
            if ($cached -gt $highest) { $highest = $cached }
        }
    }
    if ($explicitVersion) { $buildVersion = $Version }
    elseif ($versions.ContainsKey($fingerprint)) { $buildVersion = [version]$versions[$fingerprint] }
    elseif ($versions.Count -eq 0) { $buildVersion = [version]'1.0.0.0' }
    else { $buildVersion = Get-NextVersion $highest }
    $versionText = $buildVersion.ToString(4)
    $buildDate = (Get-Date).ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
    $header = @"
/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#define LT9611_VERSION_MAJOR $($buildVersion.Major)
#define LT9611_VERSION_MINOR $($buildVersion.Minor)
#define LT9611_VERSION_BUILD $($buildVersion.Build)
#define LT9611_VERSION_REVISION $($buildVersion.Revision)
"@
    [IO.File]::WriteAllText((Join-Path $metadataRoot 'lt9611_build_version.h'), $header + "`n", [Text.UTF8Encoding]::new($false))
    Write-Output "Source version: $versionText ($buildDate)"
    $kitArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($WdkPropertyFile)) {
        $propertyFile = [IO.Path]::GetFullPath($WdkPropertyFile)
        if (-not (Test-Path -LiteralPath $propertyFile -PathType Leaf)) { throw 'Missing WDK property file.' }
        $kitArguments += "/p:DirectoryBuildPropsPath=$propertyFile", '/p:WindowsTargetPlatformVersion=10.0.26100.0'
    }
    foreach ($project in @('OEMBridge', 'P8CecUde', 'OEMBridgeProp')) {
        $binaryRoot = Join-Path $buildRoot "$project/bin"
        $arguments = @(
            (Join-Path $sourceRoot "$project/$project.vcxproj"),
            '/t:Rebuild', '/m', '/nologo', '/v:minimal',
            '/p:Configuration=Release', '/p:Platform=ARM64', '/p:SignMode=Off',
            '/p:EnableInf2cat=false', '/p:SupportsPackaging=false',
            "/p:OutDir=$binaryRoot/", "/p:IntDir=$buildRoot/$project/obj/",
            "/p:Lt9611BuildVersion=$versionText", "/p:Lt9611BuildDate=$buildDate",
            '/p:Lt9611UseBuildVersion=true', "/p:Lt9611BuildMetadataDirectory=$metadataRoot"
        ) + $kitArguments
        & msbuild.exe @arguments | Tee-Object -FilePath (Join-Path $logRoot "$project.log")
        if ($LASTEXITCODE -ne 0) { throw "$project build failed: $LASTEXITCODE" }
        $extension = if ($project -eq 'OEMBridgeProp') { 'dll' } else { 'sys' }
        $binary = Join-Path $binaryRoot "$project.$extension"
        if ([Diagnostics.FileVersionInfo]::GetVersionInfo($binary).FileVersion -ne $versionText) {
            throw "$project binary version does not match $versionText."
        }
        Copy-Item -LiteralPath $binary -Destination $packageRoot
    }
    $inf = Join-Path $buildRoot 'OEMBridge/bin/OEMBridge.inf'
    $expected = '(?im)^DriverVer\s*=\s*' + [regex]::Escape($buildDate + ',' + $versionText) + '\s*$'
    if ((Get-Content -LiteralPath $inf -Raw) -notmatch $expected) { throw 'Stamped INF date/version does not match the build.' }
    Copy-Item -LiteralPath $inf -Destination $packageRoot
    if ((Get-SourceFingerprint) -ne $fingerprint) { throw 'Source files changed during the build. Rebuild the current source.' }
    $metadata = [ordered]@{ version = $versionText; date = $buildDate; source = $fingerprint }
    [IO.File]::WriteAllText((Join-Path $metadataRoot 'build-version.json'), ($metadata | ConvertTo-Json) + "`n", [Text.UTF8Encoding]::new($false))
    if (-not $explicitVersion -and -not $versions.ContainsKey($fingerprint)) {
        $versions[$fingerprint] = $versionText
        $temporary = $statePath + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
        [IO.File]::WriteAllText($temporary, ((@{ schema = 1; versions = $versions } | ConvertTo-Json -Depth 4) + "`n"), [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $statePath -PathType Leaf) {
            [IO.File]::Replace($temporary, $statePath, [System.Management.Automation.Language.NullString]::Value)
        } else {
            [IO.File]::Move($temporary, $statePath)
        }
    }
    Write-Output "Built unsigned package: $packageRoot"
} finally {
    if ($lockHeld) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
