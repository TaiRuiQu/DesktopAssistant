[CmdletBinding()]
param([switch]$Hardware)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native-tools.ps1')
$repository = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'format.ps1') -Check
$msbuild = Find-NativeMSBuild
& $msbuild (Join-Path $repository 'DesktopAssistant.sln') '/restore' '/p:Configuration=Release' '/p:Platform=x64' '/m' '/v:minimal' '/nologo'
if ($LASTEXITCODE -ne 0) { throw 'Native solution build failed.' }
$checks = Join-Path $repository 'artifacts\native\x64\Release\NativeChecks.exe'
if ($Hardware) { & $checks '--hardware' } else { & $checks }
if ($LASTEXITCODE -ne 0) { throw 'Native backend/capture checks failed.' }
& (Join-Path $repository 'tests\migration_fixture.ps1') -Executable $checks
$executable = Join-Path $repository 'artifacts\winui\x64\Release\DesktopAssistant.exe'
$output = Join-Path $repository 'artifacts\ui'
$process = Start-Process -FilePath $executable -ArgumentList @('--ui-checks', '--outputdir', ('"' + $output + '"')) -PassThru -WindowStyle Hidden
if (-not $process.WaitForExit(45000)) { $process.Kill(); throw 'Native UI checks timed out.' }
if ($process.ExitCode -ne 0) { throw "Native UI checks failed; see $output\ui-report.json." }
$report = Get-Content -LiteralPath (Join-Path $output 'ui-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $report.Passed) { throw ($report.Errors -join '; ') }
Write-Host "PASS: $($report.Checks) C++/WinRT WinUI 3 checks. Previews: $output"
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Source (Join-Path $repository 'tests\http_fixture.py') $checks
    if ($LASTEXITCODE -ne 0) { throw 'WinHTTP loopback integration failed.' }
    & $python.Source (Join-Path $repository 'tests\http_fixture.py') $executable
    if ($LASTEXITCODE -ne 0) { throw 'Native UI question/answer integration failed.' }
} else {
    Write-Warning 'Python 3 not found; skipped loopback HTTP and UI integration fixtures.'
}
