param(
    [string]$Executable = (Join-Path $PSScriptRoot 'WinShellBot.exe'),
    [switch]$Administrator,
    [string]$TestFixture = ''
)
$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).ProviderPath
if (!$TestFixture) { $TestFixture = Join-Path (Split-Path -Parent $executablePath) 'ShellBot-TestFixture.exe' }
$fixturePath = (Resolve-Path -LiteralPath $TestFixture).ProviderPath
$runRoot = Join-Path $PSScriptRoot ('test-results\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-tasks-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
$null = New-Item -ItemType Directory -Path $runRoot
$workspace = Join-Path $runRoot 'workspace'
$null = New-Item -ItemType Directory -Path $workspace
$reportPath = Join-Path $runRoot 'report.json'
$utf8 = New-Object Text.UTF8Encoding($false)
$checks = New-Object 'Collections.Generic.List[string]'
$executionProfile = if ($Administrator) { 'administrator' } else { 'user' }
$report = [ordered]@{schema_version=1;scope='native_command_tasks';status='running';started_at_utc=[DateTime]::UtcNow.ToString('o');artifact_sha256=(Get-FileHash -LiteralPath $executablePath).Hash.ToLowerInvariant();execution_profile=$executionProfile;administrator_requested=[bool]$Administrator;uac_prompt_count='not_observed_automatically';controlled_isolation_tested=$false;child_delivery='native_test_executable';test_fixture_sha256=(Get-FileHash -LiteralPath $fixturePath).Hash.ToLowerInvariant();checks=@();assertions_passed=0}
$bot = $null
$script:requestId = 0
function Save-Report {
    $report.checks = @($checks.ToArray()); $report.assertions_passed = $checks.Count
    [IO.File]::WriteAllText($reportPath,($report | ConvertTo-Json -Depth 12),$utf8)
}
function Assert-Check([bool]$Condition,[string]$Label) {
    if (!$Condition) { throw ('Assertion failed: ' + $Label) }
    $checks.Add($Label)
}
function Start-Bot {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $executablePath
    $info.Arguments = '--stdio --config "' + (Join-Path $runRoot 'config.json') + '" --data-dir "' + (Join-Path $runRoot 'state') + '"'
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8; $info.StandardErrorEncoding = $utf8
    $process = New-Object Diagnostics.Process; $process.StartInfo = $info
    if (!$process.Start()) { throw 'Cannot start ShellBot test host.' }
    return $process
}
function Invoke-Mcp([string]$Method,[hashtable]$Parameters) {
    $script:requestId++
    $Parameters['_meta'] = @{ 'io.modelcontextprotocol/protocolVersion'='2026-07-28'; 'io.modelcontextprotocol/clientCapabilities'=@{} }
    $message = @{jsonrpc='2.0';id=$script:requestId;method=$Method;params=$Parameters} | ConvertTo-Json -Depth 12 -Compress
    $bot.StandardInput.WriteLine($message); $bot.StandardInput.Flush()
    $read = $bot.StandardOutput.ReadLineAsync()
    if (!$read.Wait(60000)) { throw 'MCP response timed out, including the startup authorization window.' }
    if (!$read.Result) { throw 'ShellBot exited before returning MCP data.' }
    $response = $read.Result | ConvertFrom-Json
    Assert-Check ($response.jsonrpc -eq '2.0' -and $response.id -eq $script:requestId -and !$response.error) 'MCP response matches request'
    return $response.result
}
function Invoke-Tool([string]$Name,[hashtable]$Arguments) {
    if ($Name.StartsWith('task_')) { $Arguments['host_instance_id']=$script:hostInstance }
    $result = Invoke-Mcp 'tools/call' @{name=$Name;arguments=$Arguments}
    if ($result.isError) { throw ('Task tool failed: ' + $Name + ': ' + $result.structuredContent.error) }
    Assert-Check (!$result.isError) ('tool succeeds: ' + $Name)
    return $result.structuredContent
}
function Start-Task([string]$Key,[string]$Mode,[int]$Timeout=30000) {
    return Invoke-Tool 'task_start' @{workspace_id='test';executable=$fixturePath;argv=@($Mode);request_key=$Key;timeout_ms=$Timeout}
}
function Wait-Task([string]$TaskId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    do {
        $value = Invoke-Tool 'task_status' @{task_id=$TaskId}
        if ($value.status -ne 'running') { return $value }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Task did not finish before the acceptance deadline.'
}
function Get-Output([string]$TaskId,[string]$Stream='stdout') {
    $buffer = New-Object IO.MemoryStream
    try {
        $offset = 0
        do {
            $page = Invoke-Tool 'task_output' @{task_id=$TaskId;stream=$Stream;offset=$offset;max_bytes=16384}
            $bytes = [Convert]::FromBase64String($page.data_base64)
            Assert-Check ($page.next_offset -eq ($offset + $bytes.Length)) 'output uses raw byte offsets'
            $buffer.Write($bytes,0,$bytes.Length); $offset = $page.next_offset
            if (!$page.eof -and !$bytes.Length) { throw 'Completed task output stopped making progress.' }
        } while (!$page.eof)
        return ,$buffer.ToArray()
    } finally { $buffer.Dispose() }
}
function Stop-Bot {
    if ($bot -and !$bot.HasExited) {
        $bot.StandardInput.Close()
        if (!$bot.WaitForExit(10000)) { $bot.Kill(); $null=$bot.WaitForExit(5000); throw 'ShellBot did not stop after EOF.' }
    }
}
Save-Report
try {
    $version = & $executablePath --version
    Assert-Check ($LASTEXITCODE -eq 0 -and $version -eq 'ShellBot 0.1.0-dev.8 (C17)') 'artifact version'
    $report['artifact_version']=$version
    $os=Get-CimInstance Win32_OperatingSystem
    $report['os']=@{caption=$os.Caption;build=$os.BuildNumber;version=$os.Version}
    $configuration=@{roots=@(@{id='test';path=$workspace;read_only=$true});execution=@{enabled=$true;profile=$executionProfile}}
    [IO.File]::WriteAllText((Join-Path $runRoot 'config.json'),($configuration | ConvertTo-Json -Depth 6),$utf8)
    $bot=Start-Bot
    $capabilities=Invoke-Tool 'host_capabilities' @{}
    $script:hostInstance=$capabilities.execution_status.host_instance_id
    Assert-Check ($capabilities.execution -and $capabilities.execution_status.task_tools_available) 'configured task backend ready'
    Assert-Check (!$capabilities.execution_status.host_identity.elevated) 'MCP frontend remains unelevated'
    $agentId=$null
    if ($Administrator) {
        Assert-Check ($capabilities.execution_status.administrator_agent_alive -and $capabilities.execution_status.administrator_identity.elevated) 'administrator agent has actual elevated identity'
        $agentId=[int]$capabilities.execution_status.administrator_identity.process_id
        $report['administrator_agent_process_id']=$agentId
    }
    $identityTask=Start-Task 'identity' 'identity'
    $finished=Wait-Task $identityTask.task_id
    Assert-Check ($finished.status -eq 'completed' -and $finished.exit_code -eq 0) 'identity task completed'
    $identityOutput=$utf8.GetString((Get-Output $identityTask.task_id)) | ConvertFrom-Json
    Assert-Check ($identityOutput.elevated -eq [bool]$Administrator) 'actual child account elevation matches selected profile'
    Assert-Check (!$identityOutput.token_inherited) 'child does not inherit MCP bearer environment'
    $report['actual_child_elevated']=[bool]$identityOutput.elevated
    $escaped=Start-Task 'escaped' 'escaped'
    $null=Wait-Task $escaped.task_id
    $escapedBytes=Get-Output $escaped.task_id
    Assert-Check ($escapedBytes.Length -eq 16384 -and @($escapedBytes | Where-Object {$_ -ne 1}).Count -eq 0) 'escaped maximum output page preserves every byte'
    $escapedPage=Invoke-Tool 'task_output' @{task_id=$escaped.task_id}
    Assert-Check ($escapedPage.text_utf8.Length -eq 16384) 'escaped UTF-8 preview fits administrator communication budget'
    $null=Invoke-Tool 'task_status' @{task_id=$identityTask.task_id}
    $first=Start-Task 'append-once' 'append'
    $null=Wait-Task $first.task_id
    $second=Start-Task 'append-once' 'append'
    Assert-Check ($first.task_id -eq $second.task_id -and !$second.created_new -and [IO.File]::ReadAllText((Join-Path $workspace 'once.txt')) -eq 'ONE') 'start retry never repeats file side effect'
    $interactive=Start-Task 'stdin' 'stdin'
    $null=Invoke-Tool 'task_input' @{task_id=$interactive.task_id;input_key='line';text='ShellBot stdin';eof=$false}
    $null=Invoke-Tool 'task_input' @{task_id=$interactive.task_id;input_key='line';text='ShellBot stdin';eof=$false}
    $null=Invoke-Tool 'task_input' @{task_id=$interactive.task_id;input_key='eof';text='';eof=$true}
    $null=Wait-Task $interactive.task_id
    Assert-Check ($utf8.GetString((Get-Output $interactive.task_id)) -eq 'ShellBot stdin') 'stdin retry delivers one copy'
    $timed=Start-Task 'timeout' 'wait' 500
    Assert-Check ((Wait-Task $timed.task_id).status -eq 'timed_out') 'timeout stops task'
    $cancelled=Start-Task 'cancel' 'wait'
    $taskProcess=Get-Process -Id $cancelled.process_id
    $null=Invoke-Tool 'task_cancel' @{task_id=$cancelled.task_id}
    $null=Invoke-Tool 'task_cancel' @{task_id=$cancelled.task_id}
    Assert-Check ((Wait-Task $cancelled.task_id).status -eq 'cancelled' -and $taskProcess.WaitForExit(5000)) 'cancel stops owned process and is repeatable'
    if ($Administrator) {
        $after=Invoke-Tool 'host_capabilities' @{}
        Assert-Check ($after.execution_status.administrator_agent_starts -eq 1 -and $after.execution_status.administrator_identity.process_id -eq $agentId) 'multiple tasks reuse the same elevated agent'
    }
    $shutdown=Start-Task 'shutdown' 'wait'
    $shutdownProcess=Get-Process -Id $shutdown.process_id
    $agentProcess=if($agentId){Get-Process -Id $agentId}else{$null}
    Stop-Bot
    Assert-Check ($shutdownProcess.WaitForExit(5000)) 'host shutdown stops running task'
    if($agentProcess){Assert-Check ($agentProcess.WaitForExit(5000)) 'host shutdown stops administrator agent'}
    Assert-Check ($bot.ExitCode -eq 0 -and !$bot.StandardError.ReadToEnd()) 'host clean exit'
    Assert-Check ((Get-FileHash -LiteralPath $executablePath).Hash.ToLowerInvariant() -eq $report.artifact_sha256) 'artifact unchanged'
    Assert-Check ((Get-FileHash -LiteralPath $fixturePath).Hash.ToLowerInvariant() -eq $report.test_fixture_sha256) 'test fixture unchanged'
    $report.status='passed'
} catch {
    $report.status='failed';$report['error']=$_.Exception.Message
} finally {
    try { Stop-Bot } catch { $report.status='failed';$report['cleanup_error']=$_.Exception.Message }
    if ($bot) { $bot.Dispose() }
    $report['completed_at_utc']=[DateTime]::UtcNow.ToString('o');Save-Report
}
Write-Output ('Tasks: ' + $report.status + '. Profile: ' + $executionProfile + '. Checks: ' + $checks.Count + '.')
Write-Output ('Report and isolated test files: ' + $runRoot)
if($report.status -ne 'passed'){exit 1}
