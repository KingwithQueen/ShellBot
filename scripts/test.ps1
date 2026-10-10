param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')][string]$Platform = 'x64',
    [string]$Python = 'python',
    [switch]$SkipCloseProbe
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repoRoot "out\$Platform\$Configuration\WinShellBot.exe"
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the selected configuration before testing.' }
$interpreter = Get-Command $Python -ErrorAction Stop
if ($Platform -eq 'x64') {
    $projectReport = Join-Path $repoRoot "build\validation\$Platform-$Configuration-m1.json"
    $policyExecutable = Join-Path $repoRoot "out\$Platform\$Configuration\ShellBot-ProjectPolicyTests.exe"
    & $interpreter.Source (Join-Path $repoRoot 'tests\project_checks.py') --exe $executable --policy-exe $policyExecutable --report $projectReport
    if ($LASTEXITCODE -ne 0) { throw "Project validation failed ($LASTEXITCODE). Report: $projectReport" }
}
$suffix = if ($SkipCloseProbe) { '-core' } else { '-full' }
$report = Join-Path $repoRoot "build\validation\$Platform-$Configuration$suffix.json"
$arguments = @((Join-Path $repoRoot 'tests\smoke.py'), '--exe', $executable, '--report', $report)
if ($SkipCloseProbe) { $arguments += '--skip-close-probe' }
& $interpreter.Source @arguments
if ($LASTEXITCODE -ne 0) { throw "Validation failed ($LASTEXITCODE). Report: $report" }
Write-Output "Validation report: $report"
