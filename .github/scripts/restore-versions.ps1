# SPDX-License-Identifier: GPL-2.0-only
# Licensed under the GNU General Public License, version 2 only.
# Provided without warranty. See LICENSE for the license text.

[CmdletBinding()]
param([Parameter(Mandatory)][string]$VersionCacheDirectory)

$ErrorActionPreference = 'Stop'
$statePath = Join-Path $VersionCacheDirectory 'source-versions.json'

function Save-VersionState([string]$Text) {
    $state = $Text | ConvertFrom-Json
    if ($state.schema -ne 1 -or $null -eq $state.versions) { throw 'Invalid source-version backup.' }
    New-Item -ItemType Directory -Path $VersionCacheDirectory -Force | Out-Null
    [IO.File]::WriteAllText($statePath, $Text, [Text.UTF8Encoding]::new($false))
}

if (-not [string]::IsNullOrWhiteSpace($env:RECOVERED_VERSION_STATE)) {
    Save-VersionState $env:RECOVERED_VERSION_STATE
    return
}
if (Test-Path -LiteralPath $statePath -PathType Leaf) { return }
$headers = @{
    Accept = 'application/vnd.github+json'
    Authorization = 'Bearer ' + $env:GH_TOKEN
    'X-GitHub-Api-Version' = '2022-11-28'
}
$api = $env:GITHUB_API_URL + '/repos/' + $env:GITHUB_REPOSITORY
$branch = [Uri]::EscapeDataString($env:DEFAULT_BRANCH)
$runs = @()
foreach ($eventName in @('push', 'workflow_dispatch')) {
    $response = Invoke-RestMethod -Uri "$api/actions/workflows/build-drivers.yml/runs?branch=$branch&event=$eventName&status=success&per_page=1" -Headers $headers
    $runs += @($response.workflow_runs)
}
$previous = $runs | Where-Object { $_.head_repository.full_name -eq $env:GITHUB_REPOSITORY } | Sort-Object id -Descending | Select-Object -First 1
if ($null -eq $previous) { return }
$artifacts = Invoke-RestMethod -Uri "$api/actions/runs/$($previous.id)/artifacts?per_page=100" -Headers $headers
$backup = $artifacts.artifacts | Where-Object { $_.name -eq 'source-versions' -and -not $_.expired } | Sort-Object id -Descending | Select-Object -First 1
if ($null -eq $backup) {
    $jobs = Invoke-RestMethod -Uri "$api/actions/runs/$($previous.id)/jobs?per_page=100" -Headers $headers
    if (@($jobs.jobs.steps | Where-Object name -EQ 'Upload version-state backup').Count -eq 0) {
        return
    }
    throw 'Source-version state is unavailable. Supply the saved source-versions.json through the manual workflow input before assigning new versions.'
}
$download = Join-Path $env:RUNNER_TEMP ('lt9611-versions-' + [Guid]::NewGuid().ToString('N') + '.zip')
Invoke-WebRequest -Uri "$api/actions/artifacts/$($backup.id)/zip" -Headers $headers -OutFile $download
$archive = [IO.Compression.ZipFile]::OpenRead($download)
try {
    $entry = $archive.GetEntry('source-versions.json')
    if ($null -eq $entry -or $entry.Length -gt 10MB) { throw 'Invalid source-version backup.' }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { Save-VersionState $reader.ReadToEnd() }
    finally { $reader.Dispose() }
} finally {
    $archive.Dispose()
}
