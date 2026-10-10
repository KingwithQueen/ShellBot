param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$Administrator,
    [switch]$CancelStartup,
    [switch]$AdministratorShutdown,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
if (@($Administrator,$CancelStartup,$AdministratorShutdown).Where({ $_ }).Count -gt 1) { throw 'Choose one elevation scenario.' }
$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot "out\x64\$Configuration\WinShellBot.exe"
$mode = if ($Administrator) { 'administrator' } elseif ($CancelStartup) { 'cancel-start' } elseif ($AdministratorShutdown) { 'administrator-shutdown' } else { 'ordinary' }
$report = Join-Path $repoRoot "build\validation\x64-$Configuration-m5-$mode.json"
$arguments = @((Join-Path $PSScriptRoot 'shared_admin_checks.py'), '--exe', $exe, '--report', $report)
if ($Administrator) { $arguments += '--administrator'; Write-Output 'Allow the first UAC dialog; subsequent projects reuse the same agent.' }
if ($CancelStartup) { $arguments += '--cancel-start'; Write-Output 'Cancel the UAC dialog for this separate startup-cancellation scenario.' }
if ($AdministratorShutdown) { $arguments += '--administrator-shutdown'; Write-Output 'Allow UAC to test manager shutdown with two active elevated jobs.' }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "M5 validation failed; report: $report" }
Write-Output $report
