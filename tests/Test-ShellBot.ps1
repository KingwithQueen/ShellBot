param(
    [string]$Executable = (Join-Path $PSScriptRoot 'WinShellBot.exe'),
    [ValidateRange(1,1000)][int]$CloseProbeCalls = 100,
    [switch]$HttpOnly
)
# Windows PowerShell 5.1 / PowerShell 7. No installed development tools needed.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$testRoot = Join-Path $PSScriptRoot ('test-results\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
[void][IO.Directory]::CreateDirectory($testRoot)
$reportPath = Join-Path $testRoot 'report.json'
$utf8 = New-Object Text.UTF8Encoding($false)
$checks = New-Object 'Collections.Generic.List[string]'
$stdio = $null; $http = $null; $connection = $null
$report = [ordered]@{
    schema_version = 2; status = 'running'; core_status = $(if ($HttpOnly) { 'not_run' } else { 'running' })
    scope = $(if ($HttpOnly) { 'http_only' } else { 'core_and_http' }); http_basic_status = 'running'
    started_at_utc = [DateTime]::UtcNow.ToString('o')
    artifact_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    artifact_version = ''; os = ''; assertions_passed = 0
    close_probe = [ordered]@{ calls = $CloseProbeCalls; completed_calls = 0; failures = 0; status = 'pending'; failed_calls = @() }
    fresh_keep_alive_control = [ordered]@{ calls = $CloseProbeCalls; completed_calls = 0; failures = 0; status = 'pending'; failed_calls = @() }
}
function Save-Report {
    $report.assertions_passed = $checks.Count
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 12), $utf8)
}
function Assert-Check([bool]$Condition, [string]$Name) {
    if (!$Condition) { throw "Check failed: $Name" }
    $checks.Add($Name)
}
function Start-Bot([string[]]$Arguments, [string]$Token = '') {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    # All arguments are generated paths or hex IDs; file names cannot contain quotes.
    $info.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8; $info.StandardErrorEncoding = $utf8
    if ($Token) { $info.EnvironmentVariables['SHELLBOT_TOKEN'] = $Token }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    return [pscustomobject]@{ Process = $process; Error = $process.StandardError.ReadToEndAsync() }
}
function Utility([string[]]$Arguments, [int]$ExpectedExit = 0) {
    $bot = Start-Bot $Arguments
    try {
        $output = $bot.Process.StandardOutput.ReadToEndAsync()
        $bot.Process.StandardInput.Close()
        if (!$bot.Process.WaitForExit(10000)) { throw 'Utility timed out.' }
        Assert-Check ($bot.Process.ExitCode -eq $ExpectedExit) ('utility ' + $Arguments[0])
        return $output.Result.Trim()
    } finally {
        if (!$bot.Process.HasExited) { $bot.Process.Kill(); [void]$bot.Process.WaitForExit(5000) }
        $bot.Process.Dispose()
    }
}
function Rpc([string]$Method, $Parameters = @{}) {
    return @{ jsonrpc = '2.0'; id = 1; method = $Method; params = $Parameters }
}
function Modern([string]$Method, $Parameters = @{}) {
    $Parameters['_meta'] = @{
        'io.modelcontextprotocol/protocolVersion' = '2026-07-28'
        'io.modelcontextprotocol/clientCapabilities' = @{}
    }
    return (Rpc $Method $Parameters)
}
function Stdio-Call($Request) {
    $bytes = $utf8.GetBytes(($Request | ConvertTo-Json -Depth 15 -Compress) + "`n")
    $stdio.Process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $stdio.Process.StandardInput.BaseStream.Flush()
    $line = $stdio.Process.StandardOutput.ReadLineAsync()
    if (!$line.Wait(10000) -or !$line.Result) { throw 'Stdio response timed out or process exited.' }
    return ($line.Result | ConvertFrom-Json)
}
function Tool([string]$Name, $Arguments = @{}) {
    return (Stdio-Call (Modern 'tools/call' @{ name = $Name; arguments = $Arguments })).result
}
function Owner([string]$Command, [string]$Id, [string]$StateDirectory) {
    $arguments = @('--' + $Command)
    if ($Id) { $arguments += $Id }
    $arguments += @('--data-dir', $StateDirectory)
    return ((Utility $arguments) | ConvertFrom-Json)
}
function Close-Stdio {
    $stdio.Process.StandardInput.Close()
    if (!$stdio.Process.WaitForExit(10000)) { throw 'Stdio EOF shutdown timed out.' }
    Assert-Check ($stdio.Process.ExitCode -eq 0) 'stdio EOF shutdown'
    $stdio.Process.Dispose()
}
function Connect-Http([int]$Port) {
    $client = New-Object Net.Sockets.TcpClient
    $client.NoDelay = $true; $client.ReceiveTimeout = 10000; $client.SendTimeout = 10000
    $client.Connect('127.0.0.1', $Port)
    return $client
}
function Http-Call($Client, [int]$Port, [string]$Token, $Request, [string]$Method = '', [string]$Name = '', [bool]$Close = $false, $Trace = $null, [string]$HostHeader = '', [string]$Origin = '') {
    if ($Trace) { $Trace['phase'] = 'serialize' }
    $payload = $utf8.GetBytes(($Request | ConvertTo-Json -Depth 15 -Compress))
    if (!$HostHeader) { $HostHeader = "127.0.0.1:$Port" }
    $header = "POST /mcp HTTP/1.1`r`nHost: $HostHeader`r`nContent-Type: application/json`r`nAccept: application/json, text/event-stream`r`nContent-Length: $($payload.Length)`r`n"
    if ($Origin) { $header += "Origin: $Origin`r`n" }
    if ($Token) { $header += "Authorization: Bearer $Token`r`n" }
    if ($Method) {
        $header += "MCP-Protocol-Version: 2026-07-28`r`nMcp-Method: $Method`r`n"
        if ($Name) { $header += "Mcp-Name: $Name`r`n" }
    } else { $header += "MCP-Protocol-Version: 2025-11-25`r`n" }
    $mode = if ($Close) { 'close' } else { 'keep-alive' }
    $headerBytes = [Text.Encoding]::ASCII.GetBytes($header + "Connection: $mode`r`n`r`n")
    $frame = New-Object byte[] ($headerBytes.Length + $payload.Length)
    [Buffer]::BlockCopy($headerBytes, 0, $frame, 0, $headerBytes.Length)
    [Buffer]::BlockCopy($payload, 0, $frame, $headerBytes.Length, $payload.Length)
    $stream = $Client.GetStream()
    if ($Trace) { $Trace['phase'] = 'send'; $Trace['request_bytes'] = $frame.Length }
    $stream.Write($frame, 0, $frame.Length)
    if ($Trace) { $Trace['phase'] = 'response_headers' }
    $received = New-Object 'Collections.Generic.List[byte]'
    do {
        $b = $stream.ReadByte()
        if ($b -lt 0) { throw 'HTTP peer closed before response headers.' }
        $received.Add([byte]$b)
        if ($Trace) { $Trace['response_header_bytes'] = $received.Count }
        $count = $received.Count
        if ($count -gt 16384) { throw 'HTTP header limit exceeded.' }
    } while ($count -lt 4 -or $received[$count-4] -ne 13 -or $received[$count-3] -ne 10 -or $received[$count-2] -ne 13 -or $received[$count-1] -ne 10)
    $headers = [Text.Encoding]::ASCII.GetString($received.ToArray())
    if ($Trace) { $Trace['phase'] = 'parse_headers' }
    $statusMatch = [regex]::Match($headers, '^HTTP/1\.1 (\d{3}) ')
    $lengthMatch = [regex]::Match($headers, '(?im)^Content-Length: (\d+)\r?$')
    if (!$statusMatch.Success -or !$lengthMatch.Success) { throw 'Invalid HTTP response framing.' }
    $length = [int]$lengthMatch.Groups[1].Value
    if ($Trace) { $Trace['http_status'] = [int]$statusMatch.Groups[1].Value; $Trace['response_body_expected'] = $length }
    if ($length -gt 4194304) { throw 'HTTP response limit exceeded.' }
    $body = New-Object byte[] $length
    $offset = 0
    if ($Trace) { $Trace['phase'] = 'response_body' }
    while ($offset -lt $length) {
        $got = $stream.Read($body, $offset, $length - $offset)
        if ($got -le 0) { throw 'HTTP response ended early.' }
        $offset += $got
        if ($Trace) { $Trace['response_body_received'] = $offset }
    }
    if ($Trace) { $Trace['phase'] = 'parse_json' }
    $parsed = $utf8.GetString($body) | ConvertFrom-Json
    if ($Trace) { $Trace['phase'] = 'complete' }
    return [pscustomobject]@{ Status = [int]$statusMatch.Groups[1].Value; Body = $parsed }
}

try {
    Save-Report
    $os = Get-CimInstance Win32_OperatingSystem
    $report.os = @{ caption = $os.Caption; version = $os.Version; build = $os.BuildNumber }
    $report.artifact_version = Utility @('--version')
    Assert-Check ($report.artifact_version -eq 'ShellBot 0.1.0-dev.8 (C17)') 'artifact version'
    $unicode = ([char]0x4f60).ToString() + [char]0x597d + ' ShellBot ' + [char]::ConvertFromUtf32(0x1f600) + "`n"
    $workspace = Join-Path $testRoot ('workspace ' + [char]0x4e2d + [char]0x6587 + ' & !')
    [void][IO.Directory]::CreateDirectory($workspace)
    [IO.File]::WriteAllText((Join-Path $workspace 'sample.txt'), $unicode, $utf8)
    [IO.File]::WriteAllText((Join-Path $workspace 'utf16.txt'), $unicode, [Text.Encoding]::Unicode)
    $png = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC'
    [IO.File]::WriteAllBytes((Join-Path $workspace 'pixel.png'), [Convert]::FromBase64String($png))
    $configuration = Join-Path $testRoot 'test-config.json'
    [IO.File]::WriteAllText($configuration, (@{ roots = @(@{ id = 'workspace'; path = $workspace; read_only = $false }); http = @{ public_url = 'shellbot.example.test' } } | ConvertTo-Json -Depth 8), $utf8)
    if (!$HttpOnly) {
    $stateDir = Join-Path $testRoot 'state-stdio'
    $stdio = Start-Bot @('--stdio', '--config', $configuration, '--data-dir', $stateDir)
    $discovery = (Stdio-Call (Modern 'server/discover')).result
    Assert-Check ($discovery.resultType -eq 'complete' -and $discovery.supportedVersions -contains '2026-07-28') 'modern stdio discovery'
    Assert-Check ((Stdio-Call (Modern 'tools/list')).result.tools.Count -eq 13) 'thirteen tools'
    $capabilities = (Tool 'host_capabilities').structuredContent
    Assert-Check (!$capabilities.execution -and !$capabilities.execution_status.command_execution_available) 'command execution unavailable during identity foundation'
    Assert-Check ($capabilities.execution_status.profile -eq 'controlled' -and !$capabilities.execution_status.configured_enabled) 'execution defaults disabled controlled'
    Assert-Check (!$capabilities.execution_status.remote_approval_available -and !$capabilities.execution_status.controlled_isolation_verified) 'unfinished permission capabilities reported accurately'
    Assert-Check ($capabilities.execution_status.host_identity.process_id -eq $stdio.Process.Id) 'actual host process identity'
    Assert-Check ($capabilities.durable_audit -and $capabilities.audit_healthy -and !$capabilities.durable_approvals) 'audit persistence without permission replay'
    Assert-Check ($capabilities.public_endpoint -ceq 'https://shellbot.example.test/mcp' -and !$capabilities.oauth -and !$capabilities.tunnel_management) 'user-provided external domain without tunnel management'
    Assert-Check ((Tool 'fs_read' @{ workspace_id = 'workspace'; path = 'sample.txt' }).structuredContent.text -ceq $unicode) 'Unicode read'
    $searched = Tool 'fs_search' @{ workspace_id = 'workspace'; query = 'ShellBot'; mode = 'content' }
    Assert-Check (!$searched.isError -and $searched.structuredContent.matches.Count -eq 2) 'UTF-8 and UTF-16 content search'
    foreach ($match in $searched.structuredContent.matches) {
        Assert-Check ($match.line -eq 1 -and $match.excerpt -ceq $unicode.TrimEnd()) 'content line and Unicode excerpt'
    }
    $limitedSearch = Tool 'fs_search' @{ workspace_id = 'workspace'; query = 'ShellBot'; mode = 'content'; max_results = 1 }
    Assert-Check ($limitedSearch.structuredContent.matches.Count -eq 1 -and $limitedSearch.structuredContent.truncated) 'content result limit'
    $filenameSearch = Tool 'fs_search' @{ workspace_id = 'workspace'; query = 'sample' }
    Assert-Check ($filenameSearch.structuredContent.mode -eq 'filename' -and $filenameSearch.structuredContent.matches.Count -eq 1) 'filename search remains default'
    $image = Tool 'image_view' @{ workspace_id = 'workspace'; path = 'pixel.png' }
    Assert-Check (!$image.isError -and $image.content[1].type -eq 'image' -and $image.content[1].data -ceq $png) 'native PNG bytes'
    Assert-Check ((Tool 'fs_read' @{ workspace_id = 'workspace'; path = '../test-config.json' }).isError) 'parent traversal rejected'
    $arguments = @{ workspace_id = 'workspace'; path = 'approved.txt'; text = $unicode }
    $pending = (Tool 'fs_write' $arguments).structuredContent
    $writtenApprovalId = $pending.approval_id
    Assert-Check ($pending.status -eq 'pending' -and !(Test-Path -LiteralPath (Join-Path $workspace 'approved.txt'))) 'approval has no file side effects'
    $inspected = Owner 'inspect' $pending.approval_id $stateDir
    Assert-Check ($inspected.text -ceq $unicode) 'owner inspection exact content'
    [void](Owner 'approve' $pending.approval_id $stateDir)
    $arguments.approval_id = $pending.approval_id
    $written = Tool 'fs_write' $arguments
    Assert-Check (!$written.isError -and $written.structuredContent.status -eq 'completed') 'approved create'
    Assert-Check ([IO.File]::ReadAllText((Join-Path $workspace 'approved.txt'), $utf8) -ceq $unicode) 'created bytes decoded correctly'
    Assert-Check ((Tool 'fs_write' $arguments).structuredContent.status -eq 'completed') 'retry does not duplicate write'
    $collision = @{ workspace_id = 'workspace'; path = 'sample.txt'; text = 'must not overwrite' }
    $pending = (Tool 'fs_write' $collision).structuredContent
    [void](Owner 'approve' $pending.approval_id $stateDir)
    $collision.approval_id = $pending.approval_id
    Assert-Check ((Tool 'fs_write' $collision).isError -and [IO.File]::ReadAllText((Join-Path $workspace 'sample.txt'), $utf8) -ceq $unicode) 'existing file preserved'
    $denied = @{ workspace_id = 'workspace'; path = 'denied.txt'; text = 'denied' }
    $pending = (Tool 'fs_write' $denied).structuredContent
    [void](Owner 'deny' $pending.approval_id $stateDir)
    $denied.approval_id = $pending.approval_id
    Assert-Check ((Tool 'fs_write' $denied).structuredContent.status -eq 'denied') 'owner denial enforced'
    $source = (Tool 'fs_read' @{ workspace_id = 'workspace'; path = 'sample.txt' }).structuredContent
    Assert-Check ($source.sha256 -ceq (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.txt')).Hash.ToLowerInvariant()) 'full read returns original byte hash'
    $patchArgs = @{ workspace_id = 'workspace'; path = 'sample.txt'; search = 'ShellBot'; replacement = 'Patched'; expected_sha256 = $source.sha256 }
    $patchPending = (Tool 'fs_patch' $patchArgs).structuredContent
    $patchApprovalId = $patchPending.approval_id
    $patchInspect = Owner 'inspect' $patchApprovalId $stateDir
    Assert-Check ($patchInspect.original_text -ceq $unicode -and $patchInspect.text -ceq $unicode.Replace('ShellBot','Patched')) 'owner inspects patch before and after'
    Assert-Check ([IO.File]::ReadAllText((Join-Path $workspace 'sample.txt'), $utf8) -ceq $unicode) 'pending patch preserves original'
    [void](Owner 'approve' $patchApprovalId $stateDir)
    $patchArgs.approval_id = $patchApprovalId
    $patchDone = (Tool 'fs_patch' $patchArgs).structuredContent
    Assert-Check ($patchDone.status -eq 'completed' -and !$patchDone.atomic_save) 'approved backed-up patch'
    Assert-Check ([IO.File]::ReadAllText((Join-Path $workspace 'sample.txt'), $utf8) -ceq $unicode.Replace('ShellBot','Patched')) 'exact patched bytes'
    $backupPath = Join-Path $stateDir ('backup-' + $patchDone.backup_id + '.bin')
    Assert-Check ([IO.File]::ReadAllText($backupPath, $utf8) -ceq $unicode) 'private backup retains original'
    $backupAcl = Get-Acl -LiteralPath $backupPath
    Assert-Check $backupAcl.AreAccessRulesProtected 'protected backup ACL'
    foreach ($rule in $backupAcl.Access) {
        $sid = $rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
        Assert-Check ($sid -eq [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -or $sid -eq 'S-1-5-18') 'backup access limited to owner and SYSTEM'
    }
    Assert-Check ((Tool 'fs_patch' $patchArgs).structuredContent.status -eq 'completed') 'patch retry cached'
    $overwrite = @{ workspace_id = 'workspace'; path = 'sample.txt'; text = 'short'; overwrite = $true; expected_sha256 = (Get-FileHash -LiteralPath (Join-Path $workspace 'sample.txt')).Hash.ToLowerInvariant() }
    $overwritePending = (Tool 'fs_write' $overwrite).structuredContent
    [void](Owner 'approve' $overwritePending.approval_id $stateDir)
    $overwrite.approval_id = $overwritePending.approval_id
    Assert-Check ((Tool 'fs_write' $overwrite).structuredContent.status -eq 'completed' -and [IO.File]::ReadAllText((Join-Path $workspace 'sample.txt'), $utf8) -ceq 'short') 'explicit overwrite truncates'
    Assert-Check ((Tool 'fs_patch' @{workspace_id='workspace';path='sample.txt';search='short';replacement='x';expected_sha256=$source.sha256}).isError) 'stale original hash rejected'
    $conflictArgs = @{workspace_id='workspace';path='sample.txt';search='short';replacement='next';expected_sha256=(Get-FileHash -LiteralPath (Join-Path $workspace 'sample.txt')).Hash.ToLowerInvariant()}
    $conflictPending = (Tool 'fs_patch' $conflictArgs).structuredContent
    [void](Owner 'approve' $conflictPending.approval_id $stateDir)
    $conflictArgs.approval_id = $conflictPending.approval_id
    [IO.File]::WriteAllText((Join-Path $workspace 'sample.txt'), 'external', $utf8)
    Assert-Check ((Tool 'fs_patch' $conflictArgs).structuredContent.status -eq 'failed' -and [IO.File]::ReadAllText((Join-Path $workspace 'sample.txt'), $utf8) -ceq 'external') 'approved patch refuses intervening changes'
    $restart = @{ workspace_id = 'workspace'; path = 'restart.txt'; text = 'requires a new approval after restart' }
    $pending = (Tool 'fs_write' $restart).structuredContent
    [void](Owner 'approve' $pending.approval_id $stateDir)
    $restart.approval_id = $pending.approval_id
    Close-Stdio; $stdio = $null
    $oldJournals = @(Get-ChildItem -LiteralPath $stateDir -Filter 'audit-*.jsonl' -File)
    Assert-Check ($oldJournals.Count -eq 1) 'one journal per running instance'
    $oldJournal = $oldJournals[0].FullName
    $oldJournalHash = (Get-FileHash -LiteralPath $oldJournal -Algorithm SHA256).Hash
    $rawAudit = [IO.File]::ReadAllText($oldJournal, $utf8)
    $events = @($rawAudit -split "`n" | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
    $writeEvents = @($events | Where-Object { $_.PSObject.Properties['approval_id'] -and $_.approval_id -eq $writtenApprovalId })
    Assert-Check (($writeEvents.event -join ',') -ceq 'approval_requested,approval_approved,write_started,write_completed') 'approval/write audit chain once'
    Assert-Check (!$rawAudit.Contains($unicode.Trim()) -and !$rawAudit.Contains('requires a new approval after restart')) 'audit omits file contents'
    $sequence = 0
    foreach ($event in $events) {
        $sequence++
        Assert-Check ($event.sequence -eq $sequence -and $event.schema_version -eq 1 -and $event.version -eq '0.1.0-dev.8') 'audit sequence and version'
    }
    $acl = Get-Acl -LiteralPath $oldJournal
    $accountSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $rules = @($acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]))
    Assert-Check ($acl.AreAccessRulesProtected -and $rules.Count -gt 0) 'protected audit ACL'
    foreach ($rule in $rules) {
        Assert-Check (!$rule.IsInherited -and @($accountSid, 'S-1-5-18') -contains $rule.IdentityReference.Value -and $rule.AccessControlType -eq 'Allow') 'audit access limited to owner and SYSTEM'
    }
    $stdio = Start-Bot @('--stdio', '--config', $configuration, '--data-dir', $stateDir)
    Assert-Check ((Tool 'fs_write' $restart).isError -and !(Test-Path -LiteralPath (Join-Path $workspace 'restart.txt'))) 'restart revokes old approval'
    Close-Stdio; $stdio = $null
    Assert-Check ((Get-FileHash -LiteralPath $oldJournal -Algorithm SHA256).Hash -ceq $oldJournalHash) 'restart preserves old audit bytes'
    Assert-Check (@(Get-ChildItem -LiteralPath $stateDir -Filter 'audit-*.jsonl' -File).Count -eq 2) 'restart uses a separate journal'
    }
    # Choose an available loopback port, then start the actual native listener.
    $listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $token = Utility @('--generate-token')
    $httpState = Join-Path $testRoot 'state-http'
    $http = Start-Bot @('--http', '--config', $configuration, '--data-dir', $httpState, '--port', "$port") $token
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        try { $connection = Connect-Http $port; break } catch { Start-Sleep -Milliseconds 100 }
    } while ([DateTime]::UtcNow -lt $deadline -and !$http.Process.HasExited)
    if (!$connection) { throw 'HTTP listener did not start.' }
    $anonymous = Http-Call $connection $port '' (Rpc 'ping')
    Assert-Check ($anonymous.Status -eq 401) 'HTTP bearer required'
    $external = Http-Call -Client $connection -Port $port -Token $token -Request (Rpc 'ping') -HostHeader 'shellbot.example.test' -Origin 'https://shellbot.example.test'
    Assert-Check ($external.Status -eq 200) 'configured external Host and Origin accepted'
    $untrusted = Http-Call -Client $connection -Port $port -Token $token -Request (Rpc 'ping') -HostHeader 'untrusted.example.test'
    Assert-Check ($untrusted.Status -eq 403) 'unconfigured external Host rejected'
    $response = Http-Call $connection $port $token (Modern 'server/discover') 'server/discover'
    Assert-Check ($response.Status -eq 200 -and $response.Body.result.resultType -eq 'complete') 'modern HTTP discovery'
    for ($i = 0; $i -lt 100; ++$i) {
        $response = Http-Call $connection $port $token (Modern 'tools/call' @{ name = 'host_info' }) 'tools/call' 'host_info'
        Assert-Check ($response.Status -eq 200 -and $response.Body.result.structuredContent.version -eq '0.1.0-dev.8') 'persistent HTTP host query'
    }
    $connection.Close(); $connection = $null
    if (!$HttpOnly) { $report.core_status = 'passed' }
    # Match request/connection creation between modes. No failed call is retried.
    foreach ($mode in @('fresh_keep_alive_control', 'close_probe')) {
        $summary = $report[$mode]
        for ($i = 0; $i -lt $CloseProbeCalls; ++$i) {
            $probe = $null
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $trace = [ordered]@{ index = $i; phase = 'connect'; response_header_bytes = 0; response_body_received = 0 }
            try {
                $probe = Connect-Http $port
                $response = Http-Call $probe $port $token (Rpc 'ping') '' '' ($mode -eq 'close_probe') $trace
                if ($response.Status -ne 200) { $trace['phase'] = 'status'; throw 'Unexpected HTTP status.' }
            } catch {
                $summary.failures++
                $trace['elapsed_ms'] = $watch.ElapsedMilliseconds
                $exception = $_.Exception
                $types = @()
                while ($exception) {
                    $types += $exception.GetType().FullName
                    if ($exception -is [Net.Sockets.SocketException]) {
                        $trace['socket_error'] = $exception.SocketErrorCode.ToString()
                        $trace['native_error_code'] = $exception.NativeErrorCode
                    }
                    $exception = $exception.InnerException
                }
                $trace['error_types'] = $types
                # No raw HTTP body, headers, token, or arbitrary exception message is recorded.
                $summary.failed_calls += $trace
            } finally { if ($probe) { $probe.Close() }; $watch.Stop(); $summary.completed_calls++ }
        }
        $summary.status = if ($summary.failures -eq 0) { 'passed' } else { 'failed' }
        Save-Report
    }
    [void](Owner 'stop' '' $httpState)
    if (!$http.Process.WaitForExit(15000)) { throw 'Owner HTTP shutdown timed out.' }
    Assert-Check ($http.Process.ExitCode -eq 0) 'HTTP owner shutdown'
    $stdout = $http.Process.StandardOutput.ReadToEnd()
    Assert-Check (!$stdout -and !$http.Error.Result.Contains($token)) 'HTTP stdout and credential redaction'
    $http.Process.Dispose(); $http = $null
    if ($connection) { $connection.Close(); $connection = $null }
    $proxyState = Join-Path $testRoot 'proxy-http-data'
    $proxyConfiguration = Join-Path $testRoot 'proxy-config.json'
    [IO.File]::WriteAllText($proxyConfiguration, (@{roots=@(@{id='workspace';path=$workspace;read_only=$false})} | ConvertTo-Json -Depth 8), $utf8)
    $http = Start-Bot @('--http', '--config', $proxyConfiguration, '--data-dir', $proxyState, '--port', "$port") $token
    for ($retry = 0; $retry -lt 50; $retry++) {
        try { $connection = New-Object Net.Sockets.TcpClient; $connection.Connect('127.0.0.1', $port); break }
        catch { if ($connection) { $connection.Close(); $connection = $null }; Start-Sleep -Milliseconds 100 }
    }
    if (!$connection) { throw 'Proxy-mode host did not become ready.' }
    $proxy = Http-Call -Client $connection -Port $port -Token $token -Request (Rpc 'ping') -HostHeader 'forwarded.example.test' -Origin 'https://client.example.test'
    Assert-Check ($proxy.Status -eq 200) 'existing tunnel needs no configured domain'
    $anonymousProxy = Http-Call -Client $connection -Port $port -Token '' -Request (Rpc 'ping') -HostHeader 'forwarded.example.test'
    Assert-Check ($anonymousProxy.Status -eq 401) 'proxy mode still requires bearer'
    [void](Owner 'stop' '' $proxyState)
    if (!$http.Process.WaitForExit(15000)) { throw 'Proxy host shutdown timed out.' }
    Assert-Check ($http.Process.ExitCode -eq 0) 'proxy host stopped locally'
    $http.Process.Dispose(); $http = $null
    Assert-Check ((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant() -eq $report.artifact_sha256) 'artifact unchanged during test'
    $report.http_basic_status = 'passed'
    $report.status = if ($report.close_probe.failures -eq 0 -and $report.fresh_keep_alive_control.failures -eq 0) { 'passed' } else { 'needs_investigation' }
} catch {
    $report.status = 'failed'; $report.http_basic_status = 'failed'
    if (!$HttpOnly -and $report.core_status -ne 'passed') { $report.core_status = 'failed' }
    $report['error_type'] = $_.Exception.GetType().FullName
    $report['failure'] = $_.Exception.Message
} finally {
    if ($connection) { $connection.Close() }
    foreach ($bot in @($stdio, $http)) {
        if (!$bot) { continue }
        if (!$bot.Process.HasExited) {
            $bot.Process.StandardInput.Close()
            if (!$bot.Process.WaitForExit(1000)) { $bot.Process.Kill(); [void]$bot.Process.WaitForExit(5000) }
        }
        $bot.Process.Dispose()
    }
    $report['completed_at_utc'] = [DateTime]::UtcNow.ToString('o')
    $report['checks'] = $checks.ToArray()
    Save-Report
}
Write-Output "Overall: $($report.status). Core: $($report.core_status). Basic HTTP: $($report.http_basic_status)."
Write-Output "Connection-close diagnostic: $($report.close_probe.status), $($report.close_probe.failures)/$($report.close_probe.completed_calls) failed."
Write-Output "Fresh keep-alive control: $($report.fresh_keep_alive_control.status), $($report.fresh_keep_alive_control.failures)/$($report.fresh_keep_alive_control.completed_calls) failed."
Write-Output "Report and isolated test files: $testRoot"
if ($report.status -ne 'passed') { exit 2 }
