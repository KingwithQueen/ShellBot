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
    $managerReport = Join-Path $repoRoot "build\validation\$Platform-$Configuration-m2.json"
    & $interpreter.Source (Join-Path $repoRoot 'tests\manager_checks.py') --exe $executable --report $managerReport
    if ($LASTEXITCODE -ne 0) { throw "Manager validation failed ($LASTEXITCODE). Report: $managerReport" }
    $gatewayReport = Join-Path $repoRoot "build\validation\$Platform-$Configuration-m3.json"
    & $interpreter.Source (Join-Path $repoRoot 'tests\gateway_checks.py') --exe $executable --report $gatewayReport
    if ($LASTEXITCODE -ne 0) { throw "Gateway validation failed ($LASTEXITCODE). Report: $gatewayReport" }
    $approvalReport = Join-Path $repoRoot "build\validation\$Platform-$Configuration-m4.json"
    & $interpreter.Source (Join-Path $repoRoot 'tests\approval_checks.py') --exe $executable --report $approvalReport
    if ($LASTEXITCODE -ne 0) { throw "Approval and restricted execution validation failed ($LASTEXITCODE). Report: $approvalReport" }
}
$suffix = if ($SkipCloseProbe) { '-core' } else { '-full' }
$report = Join-Path $repoRoot "build\validation\$Platform-$Configuration$suffix.json"
$arguments = @((Join-Path $repoRoot 'tests\smoke.py'), '--exe', $executable, '--report', $report)
if ($SkipCloseProbe) { $arguments += '--skip-close-probe' }
& $interpreter.Source @arguments
if ($LASTEXITCODE -ne 0) { throw "Validation failed ($LASTEXITCODE). Report: $report" }
Write-Output "Validation report: $report"
