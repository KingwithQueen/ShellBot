param(
    [string]$ReportPrefix = 'm13-closeout',
    [string]$OutputPath = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
function Digest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$validation = Join-Path $repoRoot 'build/validation'
if (!$OutputPath) { $OutputPath = Join-Path $validation 'm13-closeout-acceptance.json' }
$stages = @('m1','m2','m3','m4','m5-ordinary','m5-administrator','m6','m7','m7-administrator','core','m8','m9','m10','m10-backend','m11','m12','m13-files','m13-files-administrator','m13-settings','settings-entry','http-close','http-boundary')
$reports = @(); $pending = @(); $artifacts = [ordered]@{}
foreach ($configuration in @('Debug','Release')) {
    $products = [ordered]@{}
    foreach ($name in @('WinShellBot.exe','ShellBot-SandboxProbe.exe','ShellBot-Launcher.exe','pdfium.dll')) {
        $path = Join-Path $repoRoot "out/x64/$configuration/$name"
        $products[$name] = [ordered]@{ path=$path; sha256=(Digest $path) }
    }
    $artifacts[$configuration] = $products
    foreach ($stage in $stages) {
        $path = Join-Path $validation "$ReportPrefix-$($configuration.ToLowerInvariant())-$stage.json"
        if (!(Test-Path -LiteralPath $path)) {
            if ($configuration -eq 'Release') { $pending += "Missing Release $stage" }
            continue
        }
        $report = Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
        $passed = Test-ValidationReportPassed $report
        $hash = if ($report.sha256) { $report.sha256 } elseif ($report.exe_sha256) { $report.exe_sha256 } else { $report.artifact.sha256 }
        if ($hash -ne $products['WinShellBot.exe'].sha256) { $passed=$false }
        if ($stage -eq 'core' -and $report.scope -ne 'full') { $passed=$false }
        if ($stage -in @('m5-administrator','m7-administrator','m13-files-administrator') -and $report.uac_attempted -ne $true) { $passed=$false }
        if ($stage -in @('m13-files','m13-files-administrator') -and $report.cross_directory_verified -ne $true) { $passed=$false }
        if (!$passed) { $pending += "Failed or stale $configuration $stage" }
        $reports += [ordered]@{ stage=$stage; subject='product'; configuration=$configuration; path=$path; sha256=(Digest $path); passed=[bool]$passed; scope=$report.scope }
    }
}
$schemaPath = Join-Path $validation 'm13-receipt-schema.json'
if (Test-Path -LiteralPath $schemaPath) {
    $schema = Get-Content -LiteralPath $schemaPath -Raw -Encoding utf8 | ConvertFrom-Json
    $schemaPassed = (Test-ValidationReportPassed $schema) -and $schema.helper_sha256 -eq (Digest (Join-Path $PSScriptRoot 'validation-report.ps1'))
    if (!$schemaPassed) { $pending += 'Receipt schema validation failed or is stale' }
    $reports += [ordered]@{stage='receipt-schema';subject='component';configuration=$null;path=$schemaPath;sha256=(Digest $schemaPath);passed=[bool]$schemaPassed;scope=$schema.scope}
} else { $pending += 'Missing receipt schema validation' }
$sources = @(& git -C $repoRoot -c core.quotepath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inventory source.' }
$rows = @($sources | ForEach-Object { $path=Join-Path $repoRoot $_; [ordered]@{ relative_path=$_; sha256=(Digest $path); bytes=(Get-Item -LiteralPath $path).Length } })
$utf8 = [Text.UTF8Encoding]::new($false)
$manifestPath = Join-Path $validation "$ReportPrefix-source-manifest.json"
[IO.File]::WriteAllText($manifestPath,(@{files=$rows} | ConvertTo-Json -Depth 8),$utf8)
$close = @($reports | Where-Object { $_.stage -eq 'http-close' -and $_.configuration -eq 'Release' -and $_.passed })
$summary = [ordered]@{
    stage='M13'; status=$(if (!$pending.Count) {'passed_local_current_candidate'} else {'development_preview_pending_environment'})
    created_utc=[DateTime]::UtcNow.ToString('o'); source_commit=(& git -C $repoRoot rev-parse HEAD).Trim()
    working_tree_dirty=[bool](& git -C $repoRoot status --porcelain)
    source_manifest=@{path=$manifestPath;sha256=(Digest $manifestPath)}
    artifacts=$artifacts; reports=$reports; pending=$pending
    connection_close=$(if ($close.Count -eq 1) {'passed_tcp_comparison'} else {'unresolved_tcp_comparison'})
    external_systems='pending'; real_mcp_client='pending'
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath),($summary | ConvertTo-Json -Depth 12),$utf8)
Write-Output "Receipt: $OutputPath"
Write-Output "Status: $($summary.status); pending: $($pending.Count)"
