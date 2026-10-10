param(
    [string]$Executable = (Join-Path $PSScriptRoot 'WinShellBot.exe'),
    [switch]$Administrator,
    [ValidateRange(2,100)][int]$StatusCalls = 20
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)
$exe = (Resolve-Path -LiteralPath $Executable).Path
$runRoot = Join-Path $PSScriptRoot ('test-results\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-permissions-' + [guid]::NewGuid().ToString('N').Substring(0,8))
[void][IO.Directory]::CreateDirectory($runRoot)
$reportPath = Join-Path $runRoot 'report.json'
$checks = New-Object 'Collections.Generic.List[string]'
$skipped = New-Object 'Collections.Generic.List[string]'
$report = [ordered]@{
    schema_version = 1; scope = 'execution_identity_foundation'; status = 'running'
    started_at_utc = [DateTime]::UtcNow.ToString('o')
    artifact_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    administrator_requested = [bool]$Administrator; uac_prompt_count = 'not_observed_automatically'
    command_execution_tested = $false; controlled_isolation_tested = $false
    checks = @(); skipped = @(); assertions_passed = 0
}
$bot = $null; $agentProcess = $null
function Save-Report {
    $report.checks = @($checks.ToArray()); $report.skipped = @($skipped.ToArray()); $report.assertions_passed = $checks.Count
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 8), $utf8)
}
function Assert-Check([bool]$Condition, [string]$Name) {
    if (!$Condition) { throw "Check failed: $Name" }
    $checks.Add($Name)
}
function Start-Bot([string[]]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    # Every generated argument lacks quotes and trailing slashes.
    $info.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8; $info.StandardErrorEncoding = $utf8
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    return $process
}
function Utility([string[]]$Arguments, [int]$ExpectedExit = 0) {
    $process = Start-Bot $Arguments
    $output = $process.StandardOutput.ReadToEndAsync(); $errorOutput = $process.StandardError.ReadToEndAsync()
    try {
        if (!$process.WaitForExit(10000)) { $process.Kill(); $process.WaitForExit(); throw 'Utility timeout.' }
        Assert-Check ($process.ExitCode -eq $ExpectedExit) 'utility expected exit code'
        $text = $output.Result
        if ($ExpectedExit -eq 0) { Assert-Check ([string]::IsNullOrEmpty($errorOutput.Result)) 'utility no diagnostic errors' }
        return $text
    } finally { $process.Dispose() }
}
function Write-Config([string]$Name, $Execution) {
    $path = Join-Path $runRoot $Name
    [IO.File]::WriteAllText($path, (@{ execution = $Execution } | ConvertTo-Json -Depth 8), $utf8)
    return $path
}
function Capabilities([int]$Id, [int]$Timeout = 10000) {
    $request = @{ jsonrpc = '2.0'; id = $Id; method = 'tools/call'; params = @{
        name = 'host_capabilities'; arguments = @{}
        _meta = @{ 'io.modelcontextprotocol/protocolVersion' = '2026-07-28'; 'io.modelcontextprotocol/clientCapabilities' = @{} }
    } }
    $payload = $utf8.GetBytes(($request | ConvertTo-Json -Depth 8 -Compress) + "`n")
    $bot.StandardInput.BaseStream.Write($payload, 0, $payload.Length); $bot.StandardInput.BaseStream.Flush()
    $read = $bot.StandardOutput.ReadLineAsync()
    if (!$read.Wait($Timeout)) { throw 'MCP status timeout.' }
    if ([string]::IsNullOrEmpty($read.Result)) { throw 'MCP host exited before returning status.' }
    $response = $read.Result | ConvertFrom-Json
    Assert-Check ($response.id -eq $Id -and $null -ne $response.result -and !$response.result.isError) 'MCP status response'
    return $response.result.structuredContent
}
function Close-Bot {
    if ($null -eq $bot) { return }
    $bot.StandardInput.Close()
    if (!$bot.WaitForExit(10000)) { $bot.Kill(); $bot.WaitForExit(); throw 'MCP host exit timeout.' }
    Assert-Check ($bot.ExitCode -eq 0) 'MCP host clean exit'
    Assert-Check ([string]::IsNullOrEmpty($bot.StandardError.ReadToEnd())) 'MCP host no diagnostic errors'
    $bot.Dispose(); $script:bot = $null
}
try {
    Save-Report
    $os = Get-CimInstance Win32_OperatingSystem
    $report['os'] = @{ caption = $os.Caption; version = $os.Version; build = $os.BuildNumber }
    $report['artifact_version'] = (Utility @('--version')).Trim()
    Assert-Check ($report.artifact_version -eq 'ShellBot 0.1.0-dev.8 (C17)') 'artifact version'
    $baseline = Utility @('--execution-status') | ConvertFrom-Json
    Assert-Check ($baseline.profile -eq 'controlled' -and !$baseline.configured_enabled -and $baseline.readiness -eq 'disabled') 'default disabled controlled profile'
    Assert-Check (!$baseline.command_execution_available -and !$baseline.task_tools_available -and !$baseline.remote_approval_available) 'unfinished capabilities remain unavailable'
    $ordinary = !$baseline.host_identity.elevated -and !$baseline.host_identity.administrator_group_enabled -and !$baseline.host_identity.restricted -and !$baseline.host_identity.appcontainer
    $report['host_elevated'] = [bool]$baseline.host_identity.elevated
    foreach ($executionProfile in @('controlled','user','administrator')) {
        $path = Write-Config ('disabled-' + $executionProfile + '.json') @{ enabled = $false; profile = $executionProfile }
        $value = Utility @('--execution-status','--config',$path) | ConvertFrom-Json
        Assert-Check ($value.profile -eq $executionProfile -and !$value.administrator_agent_started -and $value.administrator_agent_starts -eq 0) 'disabled profiles do not launch an administrator agent'
    }
    $userConfig = Write-Config 'user.json' @{ enabled = $true; profile = 'user' }
    if ($ordinary) {
        $userStatus = Utility @('--execution-status','--config',$userConfig) | ConvertFrom-Json
        Assert-Check (!$userStatus.host_identity.elevated -and $userStatus.approval_mode -eq 'preauthorized' -and !$userStatus.administrator_agent_started) 'user profile remains unelevated'
        foreach ($mode in @('always','rules')) {
            $path = Write-Config ('controlled-' + $mode + '.json') @{ enabled = $true; profile = 'controlled'; approval_mode = $mode }
            $value = Utility @('--execution-status','--config',$path) | ConvertFrom-Json
            Assert-Check ($value.restricted_token_prepared -and $value.restricted_owner_rejected -and !$value.controlled_isolation_verified) 'restricted token preparation without claiming full isolation'
            Assert-Check ($value.approval_mode -eq $mode -and !$value.command_execution_available) 'approval configuration does not grant command execution'
        }
        $bot = Start-Bot @('--stdio','--config',$path,'--data-dir',(Join-Path $runRoot 'controlled-state'))
        $value = Capabilities 1
        Assert-Check ($value.execution_status.restricted_owner_rejected -and !$value.execution) 'controlled MCP status'
        $probe = Utility @('--check-owner-boundary','--data-dir',(Join-Path $runRoot 'controlled-state')) | ConvertFrom-Json
        Assert-Check ($probe.restricted_token_rejected -and $probe.restricted_owner_command_denied) 'live owner management rejects restricted token'
        Close-Bot
    } else {
        $null = Utility @('--execution-status','--config',$userConfig) 2
        $skipped.Add('Ordinary user and restricted caller scenarios require an unelevated PowerShell session.')
    }
    $invalidConfig = Write-Config 'invalid-execution.json' @{ enabled = $true; profile = 'administrator'; approval_mode = 'always' }
    $null = Utility @('--execution-status','--config',$invalidConfig) 2
    if ($Administrator) {
        $path = Write-Config 'administrator.json' @{ enabled = $true; profile = 'administrator' }
        $bot = Start-Bot @('--stdio','--config',$path,'--data-dir',(Join-Path $runRoot 'administrator-state'))
        $value = Capabilities 1 180000
        $execution = $value.execution_status
        Assert-Check ($execution.administrator_agent_started -and $execution.administrator_agent_alive -and $execution.administrator_agent_starts -eq 1) 'administrator agent initialized once'
        Assert-Check ($execution.administrator_identity.elevated -and $execution.administrator_identity.administrator_group_enabled -and $execution.administrator_identity.integrity_rid -ge 12288) 'administrator agent actual elevated token'
        $agentId = [int]$execution.administrator_identity.process_id
        Assert-Check ($agentId -ne $bot.Id) 'administrator agent uses a separate process'
        $agentProcess = [Diagnostics.Process]::GetProcessById($agentId)
        $report['administrator_agent_process_id'] = $agentId
        $report['administrator_agent_identity_elevated'] = [bool]$execution.administrator_identity.elevated
        $report['administrator_identity_matches_host'] = ($execution.administrator_identity.user_sid -eq $execution.host_identity.user_sid)
        if ($execution.administrator_identity.user_sid -eq $execution.host_identity.user_sid) {
            $skipped.Add('Cross-account UAC identity was not exercised; administrator and host used the same account.')
        }
        for ($index = 2; $index -le $StatusCalls; $index++) {
            $value = Capabilities $index
            $execution = $value.execution_status
            Assert-Check ($execution.administrator_agent_alive -and $execution.administrator_agent_starts -eq 1 -and $execution.administrator_identity.process_id -eq $agentId) 'repeated status reuses the same elevated agent'
            Assert-Check ($value.execution -and $execution.command_execution_available -and !$execution.controlled_isolation_verified) 'administrator task backend is available without claiming complete isolation'
        }
        Close-Bot
        Assert-Check ($agentProcess.WaitForExit(10000)) 'administrator agent exits after host shutdown'
        $agentProcess.Dispose(); $agentProcess = $null
    } else { $skipped.Add('Administrator UAC, cross-account identity and agent lifetime require -Administrator acceptance.') }
    Assert-Check ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -eq $report.artifact_sha256) 'artifact unchanged'
    $report.status = 'passed'
} catch {
    $report.status = 'failed'; $report['error_type'] = $_.Exception.GetType().FullName
    Write-Warning ('Permission foundation failed: ' + $_.Exception.Message)
} finally {
    if ($null -ne $bot) {
        try { if (!$bot.HasExited) { $bot.StandardInput.Close(); if (!$bot.WaitForExit(10000)) { $bot.Kill(); $bot.WaitForExit() } } } catch { }
        $bot.Dispose()
    }
    if ($null -ne $agentProcess) { $agentProcess.Dispose() }
    $report['completed_at_utc'] = [DateTime]::UtcNow.ToString('o'); Save-Report
}
Write-Output ('Permission foundation: ' + $report.status + '. Checks: ' + $checks.Count + '. Skipped: ' + $skipped.Count + '.')
Write-Output ('Report and isolated test files: ' + $runRoot)
if ($report.status -ne 'passed') { exit 1 }
