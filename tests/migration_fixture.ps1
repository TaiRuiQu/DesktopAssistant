param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$fixtureRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts\migration-fixture'
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
function Protect-Fixture([string]$Value) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
    [Convert]::ToBase64String([Security.Cryptography.ProtectedData]::Protect($bytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser))
}
function Unprotect-Fixture([string]$Value) {
    $bytes = [Convert]::FromBase64String($Value)
    [Text.Encoding]::UTF8.GetString([Security.Cryptography.ProtectedData]::Unprotect($bytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser))
}
$document = @{ Version = 1; Settings = @{ AnswerModel = 'legacy-model'; ApiKey = '' }; ProtectedApiKey = Protect-Fixture 'legacy-fixture-key' }
[IO.File]::WriteAllText((Join-Path $fixtureRoot 'settings.json'), ($document | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
$history = '[{"Question":"legacy question","Answer":"legacy answer","DesktopTranscript":"legacy desktop","CreatedAt":"2026-11-10T00:00:00+00:00"}]'
[IO.File]::WriteAllText((Join-Path $fixtureRoot 'history.dat'), (Protect-Fixture $history), [Text.UTF8Encoding]::new($false))
& $Executable '--migration-fixture' $fixtureRoot
if ($LASTEXITCODE -ne 0) { throw 'C++ could not read legacy data.' }
$saved = Get-Content -LiteralPath (Join-Path $fixtureRoot 'settings.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ((Unprotect-Fixture $saved.ProtectedApiKey) -ne 'native-fixture-key') { throw '.NET could not decrypt C++ key.' }
$savedHistory = Unprotect-Fixture ([IO.File]::ReadAllText((Join-Path $fixtureRoot 'history.dat'))) | ConvertFrom-Json
if ($savedHistory.Count -ne 2 -or $savedHistory[1].Question -ne 'native question') { throw '.NET could not read C++ history.' }
Write-Host 'PASS: original .NET and native C++ settings/history are compatible in both directions.'
