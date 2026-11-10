[CmdletBinding()]
param(
    [ValidateSet('win-x64', 'win-arm64')]
    [string]$Runtime = 'win-x64',
    [switch]$Separate
)

$ErrorActionPreference = 'Stop'
$repositoryPath = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $repositoryPath 'src\DesktopAssistant\DesktopAssistant.vcxproj'
. (Join-Path $PSScriptRoot 'native-tools.ps1')
$msbuild = Find-NativeMSBuild
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $repositoryPath 'artifacts'))
$distributionName = if ($Separate) { 'desktop-assistant-winui' } else { 'desktop-assistant' }
$distributionRoot = [IO.Path]::GetFullPath((Join-Path $artifactRoot $distributionName))
$publishPath = [IO.Path]::GetFullPath((Join-Path $distributionRoot $Runtime))
$buildId = [Guid]::NewGuid().ToString('N')
$stagingPath = [IO.Path]::GetFullPath((Join-Path $distributionRoot ".staging-$Runtime-$buildId"))
$backupPath = [IO.Path]::GetFullPath((Join-Path $distributionRoot ".previous-$Runtime-$buildId"))
$platform = if ($Runtime -eq 'win-arm64') { 'ARM64' } else { 'x64' }

function Assert-ArtifactPath([string]$Candidate) {
    $fullPath = [IO.Path]::GetFullPath($Candidate)
    $prefix = $artifactRoot.TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing a file operation outside the artifacts directory: $fullPath"
    }
    # Refuse junction/symlink roots and parent directories before recursive work.
    $ancestor = $fullPath
    while ($ancestor.StartsWith($artifactRoot, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing a file operation through a reparse point: $ancestor"
            }
        }
        $parent = Split-Path -Parent $ancestor
        if ($parent -eq $ancestor -or [string]::IsNullOrWhiteSpace($parent)) { break }
        $ancestor = $parent
    }
    return $fullPath
}

$expectedPublishPath = [IO.Path]::GetFullPath((Join-Path $distributionRoot $Runtime))
if (-not $publishPath.Equals($expectedPublishPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The publish target does not match the selected runtime directory.'
}
$null = Assert-ArtifactPath $publishPath
$null = Assert-ArtifactPath $stagingPath
$null = Assert-ArtifactPath $backupPath

& $msbuild $projectPath '/restore' '/p:Configuration=Release' "/p:Platform=$platform" '/m' '/v:minimal' '/nologo'
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code $LASTEXITCODE. The existing distribution was preserved."
}
New-Item -ItemType Directory -Path $stagingPath -Force | Out-Null
$buildOutput = Join-Path $repositoryPath "artifacts\winui\$platform\Release"
Get-ChildItem -LiteralPath $buildOutput | Where-Object { $_.Extension -notin @('.pdb', '.ilk', '.exp', '.lib') } | Copy-Item -Destination $stagingPath -Recurse -Force
$licensesPath = Join-Path $stagingPath 'licenses'
New-Item -ItemType Directory -Path $licensesPath -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryPath 'src\DesktopAssistant\ThirdParty\md4c\LICENSE.md') -Destination (Join-Path $licensesPath 'md4c.txt')

$stagedExecutable = Join-Path $stagingPath 'DesktopAssistant.exe'
if (-not (Test-Path -LiteralPath $stagedExecutable -PathType Leaf)) {
    throw "The staged executable was not found: $stagedExecutable. The existing distribution was preserved."
}

# Swap whole directories so binaries removed by a framework migration cannot
# remain in the new distribution. Restore the old directory if the swap fails.
$oldMoved = $false
try {
    $null = Assert-ArtifactPath $publishPath
    $null = Assert-ArtifactPath $stagingPath
    $null = Assert-ArtifactPath $backupPath
    if (Test-Path -LiteralPath $publishPath) {
        Move-Item -LiteralPath $publishPath -Destination $backupPath -ErrorAction Stop
        $oldMoved = $true
    }
    Move-Item -LiteralPath $stagingPath -Destination $publishPath -ErrorAction Stop
} catch {
    $failure = $_
    if ($oldMoved -and -not (Test-Path -LiteralPath $publishPath)) {
        $null = Assert-ArtifactPath $backupPath
        $null = Assert-ArtifactPath $publishPath
        Move-Item -LiteralPath $backupPath -Destination $publishPath -ErrorAction Stop
    }
    throw "Could not replace the distribution. Close desktop assistant and retry. The previous distribution was preserved or restored. $failure"
}
if ($oldMoved) {
    $null = Assert-ArtifactPath $backupPath
    try { Remove-Item -LiteralPath $backupPath -Recurse -Force -ErrorAction Stop }
    catch { Write-Warning "The new distribution is ready, but an old backup could not be removed: $backupPath" }
}
$executablePath = Join-Path $publishPath 'DesktopAssistant.exe'

Write-Host "Published desktop assistant: $executablePath"
Write-Host 'C++/WinRT application with WinUI 3. Windows App SDK runtime files are included.'
Write-Host 'Copy the entire directory, including DLLs and resources. No .NET runtime is required.'
