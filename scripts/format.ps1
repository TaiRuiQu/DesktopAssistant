[CmdletBinding()]
param(
    [switch]$Check,
    [string]$ClangFormat = 'clang-format'
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$requiredVersion = [IO.File]::ReadAllText((Join-Path $repository '.clang-format-version')).Trim()
$formatter = Get-Command $ClangFormat -ErrorAction SilentlyContinue
if (-not $formatter) {
    throw "clang-format $requiredVersion is required. Install LLVM, or run: python -m pip install clang-format==$requiredVersion"
}
$version = & $formatter.Source '--version'
if ($LASTEXITCODE -ne 0 -or $version -notmatch ('\b' + [regex]::Escape($requiredVersion) + '\b')) {
    throw "Expected clang-format $requiredVersion. Found: $version"
}

# Limit formatting to handwritten C++ sources. Generated projections, NuGet
# packages, and build output must never be rewritten.
$files = @(
    foreach ($folder in @('src', 'tests')) {
        Get-ChildItem -LiteralPath (Join-Path $repository $folder) -Recurse -File |
            Where-Object {
                $_.Extension -in @('.cpp', '.h', '.hpp', '.c', '.cc', '.cxx') -and
                $_.FullName -notmatch '[\\/](bin|obj|Generated Files|ThirdParty)[\\/]'
            } |
            Select-Object -ExpandProperty FullName
    }
) | Sort-Object

if ($files.Count -eq 0) { throw 'No handwritten C++ files were found.' }
$arguments = @('--style=file', '--fallback-style=none')
if ($Check) { $arguments += @('--dry-run', '--Werror', '--ferror-limit=1') }
else { $arguments += '-i' }
& $formatter.Source @arguments @files
if ($LASTEXITCODE -ne 0) {
    throw 'C++ formatting check failed. Run scripts/format.ps1 to apply the repository style.'
}
$action = if ($Check) { 'Checked' } else { 'Formatted' }
Write-Host "$action $($files.Count) C++ files with clang-format $requiredVersion."
