param([string]$AcceptancePath = '')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (!$AcceptancePath) { $AcceptancePath = Join-Path $repoRoot 'build/validation/m7-local-acceptance.json' }
$acceptance = Get-Content -LiteralPath $AcceptancePath -Raw | ConvertFrom-Json
$exe = Join-Path $repoRoot 'out/x64/Release/WinShellBot.exe'
$probe = Join-Path $repoRoot 'out/x64/Release/ShellBot-SandboxProbe.exe'
function Digest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$exeHash = Digest $exe
if ($acceptance.stage -ne 'M7' -or $acceptance.status -ne 'passed_local_regression_ui_and_uac' -or
    $acceptance.artifacts.Release.'WinShellBot.exe'.sha256 -ne $exeHash -or
    $acceptance.artifacts.Release.'ShellBot-SandboxProbe.exe'.sha256 -ne (Digest $probe)) { throw 'Current M7 executable and product probe require matching local acceptance.' }
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -ne $acceptance.source_commit) { throw 'Acceptance source commit differs from the current checkout.' }
if ((Digest $acceptance.source_manifest.path) -ne $acceptance.source_manifest.sha256) { throw 'Source manifest changed.' }
$source = Get-Content -LiteralPath $acceptance.source_manifest.path -Raw | ConvertFrom-Json
$prefix = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
foreach ($entry in $source.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $repoRoot $entry.relative_path))
    if (!$path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or (Digest $path) -ne $entry.sha256) { throw "Source changed after acceptance: $($entry.relative_path)" }
}
$currentSources = @(& git -C $repoRoot -c core.quotepath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)
if ($LASTEXITCODE -ne 0 -or (Compare-Object $currentSources @($source.files.relative_path | Sort-Object -Unique))) { throw 'Source file set differs from the accepted snapshot.' }
$releaseReports = @($acceptance.reports | Where-Object { $_.configuration -eq 'Release' })
foreach ($stage in @('m1','m2','m3','m4','m5-ordinary','m6','m7','core')) {
    $reference = @($releaseReports | Where-Object { $_.stage -eq $stage })
    if ($reference.Count -ne 1 -or !$reference[0].matched_final_artifact) { throw "Missing final Release report: $stage" }
}
$references = @($releaseReports) + @($acceptance.interactive_reports)
foreach ($reference in $references) {
    if ((Digest $reference.path) -ne $reference.sha256) { throw 'Validation receipt changed.' }
    $report = Get-Content -LiteralPath $reference.path -Raw | ConvertFrom-Json
    $digest = if ($report.sha256) { $report.sha256 } else { $report.artifact.sha256 }
    if (($report.passed -ne $true -and $report.status -ne 'passed') -or $digest -ne $exeHash) { throw 'Validation must pass and match the current executable.' }
}
foreach ($stage in @('m5-administrator','m7-administrator')) {
    $reference = @($acceptance.interactive_reports | Where-Object { $_.stage -eq $stage })
    if ($reference.Count -ne 1 -or !$reference[0].matched_final_artifact) { throw "Missing current native UAC report: $stage" }
    $report = Get-Content -LiteralPath $reference[0].path -Raw | ConvertFrom-Json
    if (!$report.uac_attempted) { throw 'Administrator evidence must include actual UAC execution.' }
}
$core = Get-Content -LiteralPath ($releaseReports | Where-Object { $_.stage -eq 'core' }).path -Raw | ConvertFrom-Json
if ($core.scope -ne 'core_except_connection_close') { throw 'Candidate declares the known connection-close exclusion explicitly.' }
$versionText = & $exe --version
if ($LASTEXITCODE -ne 0 -or $versionText -notmatch '^ShellBot ([0-9A-Za-z.\-]+) \(C17\)$') { throw 'Cannot identify program version.' }
$version = $Matches[1]
$name = "ShellBot-$version-windows-x64-candidate-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$stagePath = Join-Path $repoRoot "dist/$name"
$archive = "$stagePath.zip"
if ((Test-Path -LiteralPath $stagePath) -or (Test-Path -LiteralPath $archive)) { throw 'Candidate destination exists; preserve it.' }
[void][IO.Directory]::CreateDirectory($stagePath)
$files = [ordered]@{
    'WinShellBot.exe' = $exe
    'ShellBot-SandboxProbe.exe' = $probe
    'manager.example.json' = (Join-Path $repoRoot 'config/manager.example.json')
    'PROJECT_MANAGEMENT_UI.md' = (Join-Path $repoRoot 'docs/PROJECT_MANAGEMENT_UI.md')
    'PROJECT_CONFIGURATION.md' = (Join-Path $repoRoot 'docs/PROJECT_CONFIGURATION.md')
    'PROJECT_MANAGER.md' = (Join-Path $repoRoot 'docs/PROJECT_MANAGER.md')
    'REMOTE_APPROVALS.md' = (Join-Path $repoRoot 'docs/REMOTE_APPROVALS.md')
    'SHARED_ADMINISTRATOR.md' = (Join-Path $repoRoot 'docs/SHARED_ADMINISTRATOR.md')
    'EXECUTION_PERMISSIONS.md' = (Join-Path $repoRoot 'docs/EXECUTION_PERMISSIONS.md')
    'HANDOFF_20261011_M7.md' = (Join-Path $repoRoot 'docs/HANDOFF_20261011_M7.md')
    'LICENSE.txt' = (Join-Path $repoRoot 'LICENSE.txt')
    'licenses/yyjson-MIT.txt' = (Join-Path $repoRoot 'third_party/yyjson/LICENSE')
    'licenses/CivetWeb-MIT.txt' = (Join-Path $repoRoot 'third_party/civetweb/LICENSE.md')
    'validation/local-acceptance.json' = $AcceptancePath
    'validation/source-manifest.json' = $acceptance.source_manifest.path
}
foreach ($reference in $references) { $files['validation/' + (Split-Path -Leaf $reference.path)] = $reference.path }
foreach ($entry in $files.GetEnumerator()) {
    $target = Join-Path $stagePath $entry.Key
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
    Copy-Item -LiteralPath $entry.Value -Destination $target
}
$utf8 = New-Object Text.UTF8Encoding($false)
$start = @'
@echo off
if not exist "%~dp0projects.local.json" (
  echo Copy manager.example.json to projects.local.json and configure your directories first.
  echo See PROJECT_MANAGEMENT_UI.md. Set project and owner token environment variables in this terminal.
  exit /b 2
)
"%~dp0WinShellBot.exe" --manager --http --projects "%~dp0projects.local.json" --manager-data-dir "%~dp0Data\Manager"
'@
$open = @'
@echo off
"%~dp0WinShellBot.exe" --manage --manager-data-dir "%~dp0Data\Manager"
'@
[IO.File]::WriteAllText((Join-Path $stagePath 'Start-Manager.cmd'), $start.Replace("`r`n", "`n").Replace("`n", "`r`n"), $utf8)
[IO.File]::WriteAllText((Join-Path $stagePath 'Open-Management.cmd'), $open.Replace("`r`n", "`n").Replace("`n", "`r`n"), $utf8)
$manifest = [ordered]@{
    version = $version; purpose = 'M7 local candidate; independent systems, remote proxy and real MCP clients pending'
    compiler_language = 'C17'; runtime = 'static MSVC CRT'; source_commit = $commit
    source_local_changes = $acceptance.working_tree_dirty; source_manifest_sha256 = $acceptance.source_manifest.sha256
    executable_sha256 = $exeHash; product_probe_sha256 = (Digest $probe)
    acceptance_sha256 = (Digest $AcceptancePath); core_scope = $core.scope
    uac_dialog_count = 'not_observed_automatically'; bundled_data = $false; bundled_credentials = $false
    windows_11_local = 'passed'; windows_10_independent = 'pending'; server_2025_independent = 'pending'
    actual_mcp_client = 'pending'; remote_device_proxy = 'pending'; connection_close = 'unresolved'
    dependencies = @('Windows system DLLs', 'Common Controls v6', 'ShellBot-SandboxProbe.exe for LPAC')
    license_status = 'Third-party notices included; owner must complete project copyright before formal release'
}
[IO.File]::WriteAllText((Join-Path $stagePath 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8), $utf8)
$hashes = @(Get-ChildItem -LiteralPath $stagePath -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($stagePath.Length + 1).Replace('\','/'); sha256 = (Digest $_.FullName) }
})
if ($hashes.path -match '(^|/)Data(/|$)' -or $hashes.path -contains 'projects.local.json' -or $hashes.path -contains 'ShellBot-TestFixture.exe') { throw 'Candidate contains runtime or development data.' }
[IO.File]::WriteAllText((Join-Path $stagePath 'SHA256SUMS.json'), (ConvertTo-Json -InputObject $hashes -Depth 4), $utf8)
Compress-Archive -LiteralPath $stagePath -DestinationPath $archive -CompressionLevel Optimal
$archiveHash = Digest $archive
[IO.File]::WriteAllText("$archive.sha256", "$archiveHash  $name.zip`n", $utf8)
$receipt = [ordered]@{ stage = 'M7'; status = 'created_candidate_pending_extracted_runtime_check'; archive = $archive; archive_sha256 = $archiveHash; directory = $stagePath; source_commit = $commit; source_manifest_sha256 = $acceptance.source_manifest.sha256; executable_sha256 = $exeHash; product_probe_sha256 = (Digest $probe); files = $hashes.Count }
[IO.File]::WriteAllText((Join-Path $repoRoot 'build/validation/m7-package-receipt.json'), ($receipt | ConvertTo-Json -Depth 5), $utf8)
Write-Output "Candidate: $archive"
Write-Output "SHA256: $archiveHash"
