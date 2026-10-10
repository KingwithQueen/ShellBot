param(
    [Parameter(Mandatory = $true)][string]$PublicUrl,
    [string]$TokenEnv = 'SHELLBOT_TOKEN',
    [ValidateRange(1,120)][int]$TimeoutSeconds = 15,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'endpoint-test-results')
)
# Windows PowerShell 5.1 / PowerShell 7. Tests an existing host; never starts or stops it.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.Net.Http
$urlText = $PublicUrl.Trim()
if ($urlText -notmatch '^[A-Za-z][A-Za-z0-9+.-]*://') { $urlText = 'https://' + $urlText }
try { $url = [Uri]$urlText } catch { throw 'PublicUrl must be an HTTP(S) origin or its /mcp address.' }
if (!$url.IsAbsoluteUri -or $url.Scheme -notin @('http','https') -or $url.UserInfo -or
    $url.Query -or $url.Fragment -or $url.AbsolutePath -notin @('/','/mcp')) {
    throw 'PublicUrl must be an HTTP(S) origin or its /mcp address, without credentials or query parameters.'
}
$origin = $url.GetLeftPart([UriPartial]::Authority)
$token = [Environment]::GetEnvironmentVariable($TokenEnv, 'Process')
$runRoot = Join-Path $OutputDirectory ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
[void][IO.Directory]::CreateDirectory($runRoot)
$reportPath = Join-Path $runRoot 'report.json'
$utf8 = New-Object Text.UTF8Encoding($false)
$requests = New-Object 'Collections.Generic.List[object]'
$checks = New-Object 'Collections.Generic.List[string]'
$report = [ordered]@{
    schema_version = 1; scope = 'existing_endpoint_read_only'; status = 'running'
    started_at_utc = [DateTime]::UtcNow.ToString('o'); endpoint = ($origin + '/mcp')
    token_provided = [bool]$token; assertions_passed = 0; requests = $requests; checks = $checks
}
$handler = New-Object Net.Http.HttpClientHandler
$handler.AllowAutoRedirect = $false
$handler.UseCookies = $false
$client = New-Object Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromSeconds($TimeoutSeconds)
$client.MaxResponseContentBufferSize = 262144
$phase = 'anonymous_bearer_required'
$exitCode = 1
function Assert-Endpoint([bool]$Condition, [string]$Name) {
    if (!$Condition) { throw 'Endpoint validation failed.' }
    $checks.Add($Name)
}
function Send-Endpoint([string]$Name, [string]$Method, [string]$Path, $Payload = $null, [bool]$Authenticated = $true) {
    $entry = [ordered]@{ name = $Name; status_code = 0; elapsed_ms = 0; response_bytes = 0; outcome = 'running' }
    $requests.Add($entry)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $request = $null; $response = $null
    try {
        $request = New-Object Net.Http.HttpRequestMessage([Net.Http.HttpMethod]::new($Method), ($origin + $Path))
        [void]$request.Headers.TryAddWithoutValidation('Accept', 'application/json, text/event-stream')
        [void]$request.Headers.TryAddWithoutValidation('Origin', $origin)
        if ($Authenticated) {
            $request.Headers.Authorization = New-Object Net.Http.Headers.AuthenticationHeaderValue('Bearer', $token)
            [void]$request.Headers.TryAddWithoutValidation('MCP-Protocol-Version', '2025-11-25')
        }
        if ($null -ne $Payload) {
            $json = ConvertTo-Json -InputObject $Payload -Depth 12 -Compress
            $request.Content = New-Object Net.Http.StringContent($json, $utf8, 'application/json')
        }
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        $entry.status_code = [int]$response.StatusCode
        $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        $entry.response_bytes = $utf8.GetByteCount($body)
        $entry.outcome = 'received'
        return [pscustomobject]@{ Code = $entry.status_code; Body = $body }
    } catch {
        # Arbitrary exception messages, response bodies, and headers may contain secrets.
        $entry.outcome = 'transport_failed'
        $entry.error_type = $_.Exception.GetType().Name
        throw 'Endpoint transport failed.'
    } finally {
        $entry.elapsed_ms = $watch.ElapsedMilliseconds
        if ($null -ne $response) { $response.Dispose() }
        if ($null -ne $request) { $request.Dispose() }
    }
}
function Endpoint-Rpc([int]$Id, [string]$Method, $Parameters = @{}) {
    $response = Send-Endpoint $Method 'POST' '/mcp' @{ jsonrpc = '2.0'; id = $Id; method = $Method; params = $Parameters }
    Assert-Endpoint ($response.Code -eq 200) ($Method + ' HTTP 200')
    $rpc = $response.Body | ConvertFrom-Json
    Assert-Endpoint ($rpc.jsonrpc -ceq '2.0' -and $rpc.id -eq $Id -and
        $null -eq $rpc.PSObject.Properties['error'] -and $null -ne $rpc.PSObject.Properties['result']) ($Method + ' JSON-RPC result')
    return $rpc.result
}
try {
    $anonymous = Send-Endpoint $phase 'POST' '/mcp' @{ jsonrpc = '2.0'; id = 1; method = 'ping'; params = @{} } $false
    Assert-Endpoint ($anonymous.Code -eq 401) 'anonymous request requires bearer'
    $denial = $anonymous.Body | ConvertFrom-Json
    Assert-Endpoint ($denial.error -ceq 'Bearer token required.') 'denial identifies ShellBot'
    if (!$token) {
        $report.status = 'needs_token'; $exitCode = 2
    } else {
        $phase = 'authenticated_health'
        $healthResponse = Send-Endpoint $phase 'GET' '/healthz'
        Assert-Endpoint ($healthResponse.Code -eq 200) 'authenticated health HTTP 200'
        $health = $healthResponse.Body | ConvertFrom-Json
        Assert-Endpoint ($health.ready -eq $true) 'host ready'
        $phase = 'initialize'
        $init = Endpoint-Rpc 2 'initialize' @{
            protocolVersion = '2025-11-25'; capabilities = @{}
            clientInfo = @{ name = 'ShellBot endpoint probe'; version = '1' }
        }
        Assert-Endpoint ($init.protocolVersion -ceq '2025-11-25' -and $init.serverInfo.name -ceq 'ShellBot') 'MCP initialization'
        Assert-Endpoint ($init.serverInfo.version -ceq $health.version) 'health and MCP version agree'
        $phase = 'notifications/initialized'
        $notification = Send-Endpoint $phase 'POST' '/mcp' @{ jsonrpc = '2.0'; method = $phase; params = @{} }
        Assert-Endpoint ($notification.Code -eq 202) 'initialized notification accepted'
        $phase = 'tools/list'
        $listing = Endpoint-Rpc 3 $phase
        $names = @($listing.tools | ForEach-Object { $_.name })
        Assert-Endpoint ($names -ccontains 'host_info' -and $names -ccontains 'host_capabilities') 'host tools available'
        $report.tool_count = $names.Count
        $phase = 'host_info'
        $info = Endpoint-Rpc 4 'tools/call' @{ name = $phase; arguments = @{} }
        Assert-Endpoint (!$info.isError -and $info.structuredContent.name -ceq 'ShellBot' -and
            $info.structuredContent.version -ceq $health.version) 'host_info tool result'
        $phase = 'host_capabilities'
        $capabilityResult = Endpoint-Rpc 5 'tools/call' @{ name = $phase; arguments = @{} }
        Assert-Endpoint (!$capabilityResult.isError) 'host_capabilities tool result'
        $capability = $capabilityResult.structuredContent
        Assert-Endpoint ($capability.public_endpoint -ceq ($origin + '/mcp')) 'configured public endpoint matches'
        Assert-Endpoint (!$capability.oauth -and !$capability.tunnel_management) 'external environment scope'
        $report.status = 'passed'; $exitCode = 0
    }
} catch {
    $report.status = 'failed'; $report.failed_check = $phase
    $report.error_type = $_.Exception.GetType().Name
} finally {
    $client.Dispose(); $handler.Dispose(); $token = $null
    $report.assertions_passed = $checks.Count
    $report.completed_at_utc = [DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($reportPath, (ConvertTo-Json -InputObject $report -Depth 10), $utf8)
}
Write-Output ('Endpoint: ' + $report.status + '. Checks passed: ' + $report.assertions_passed + '.')
if ($report.status -eq 'needs_token') { Write-Output ('Set the running host token in environment variable ' + $TokenEnv + ' and rerun. Do not send the token.') }
if ($report.status -eq 'failed') { Write-Output ('Failed stage: ' + $phase + '. Check the report for HTTP status or transport failure.') }
Write-Output ('Report: ' + $reportPath)
exit $exitCode
