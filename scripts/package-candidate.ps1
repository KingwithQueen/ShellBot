param([string]$AcceptancePath = '', [switch]$Preview)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
if (!$AcceptancePath) { $AcceptancePath = Join-Path $repoRoot 'build/validation/m13-local-acceptance.json' }
function Digest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$acceptance = Get-Content -LiteralPath $AcceptancePath -Raw -Encoding utf8 | ConvertFrom-Json
$exe = Join-Path $repoRoot 'out/x64/Release/WinShellBot.exe'
$exeHash = Digest $exe
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $acceptance.stage -ne 'M13' -or $acceptance.source_commit -ne $commit) { throw 'Current M13 source acceptance is required.' }
if (!$Preview -and $acceptance.status -ne 'passed_local_current_candidate') { throw 'Current runtime/UI/UAC/HTTP acceptance is incomplete. Only an explicitly labelled -Preview may be packaged.' }
if (!$Preview -and (@($acceptance.pending).Count -ne 0 -or $acceptance.connection_close -ne 'passed_tcp_comparison')) { throw 'Unresolved runtime/HTTP items cannot be labelled as a validated candidate.' }
if ($Preview -and $acceptance.status -notin @('passed_local_current_candidate','development_preview_pending_environment')) { throw 'Unknown preview acceptance state.' }
$products = @('WinShellBot.exe','ShellBot-SandboxProbe.exe','ShellBot-Launcher.exe','pdfium.dll')
foreach ($name in $products) {
    if ($acceptance.artifacts.Release.$name.sha256 -ne (Digest (Join-Path $repoRoot "out/x64/Release/$name"))) { throw "Changed or unaccepted product: $name" }
}
$pdfium = Get-Content -LiteralPath (Join-Path $repoRoot 'third_party/pdfium/provenance.json') -Raw -Encoding utf8 | ConvertFrom-Json
$pdfiumAsset = @($pdfium.assets | Where-Object architecture -eq 'x64')
if ($pdfiumAsset.Count -ne 1 -or (Digest (Join-Path $repoRoot 'out/x64/Release/pdfium.dll')) -ne $pdfiumAsset[0].dll_sha256) { throw 'PDFium differs from the pinned x64 dependency.' }
if ((Digest $acceptance.source_manifest.path) -ne $acceptance.source_manifest.sha256) { throw 'Source manifest changed.' }
$source = Get-Content -LiteralPath $acceptance.source_manifest.path -Raw -Encoding utf8 | ConvertFrom-Json
$prefix = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
foreach ($entry in $source.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $repoRoot $entry.relative_path))
    if (!$path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or (Digest $path) -ne $entry.sha256) { throw "Source changed after acceptance: $($entry.relative_path)" }
}
$currentSources = @(& git -C $repoRoot -c core.quotepath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)
if ($LASTEXITCODE -ne 0 -or (Compare-Object $currentSources @($source.files.relative_path | Sort-Object -Unique))) { throw 'Source file set differs from the accepted snapshot.' }
foreach ($reference in $acceptance.reports) {
    if ((Digest $reference.path) -ne $reference.sha256) { throw "Validation receipt changed: $($reference.stage)" }
    $report = Get-Content -LiteralPath $reference.path -Raw -Encoding utf8 | ConvertFrom-Json
    if ($reference.subject -eq 'product') {
        if ($reference.configuration -notin @('Debug','Release')) { throw 'Unknown product report configuration.' }
        $hash = if ($report.sha256) { $report.sha256 } elseif ($report.exe_sha256) { $report.exe_sha256 } else { $report.artifact.sha256 }
        $reportExeHash = Digest (Join-Path $repoRoot "out/x64/$($reference.configuration)/WinShellBot.exe")
        if ($hash -ne $reportExeHash) { throw "Receipt does not match the current product: $($reference.stage)" }
    }
}
if (!$Preview) {
    $schemaReferences = @($acceptance.reports | Where-Object { $_.stage -eq 'receipt-schema' -and $_.subject -eq 'component' })
    if ($schemaReferences.Count -ne 1) { throw 'Receipt schema validation is required.' }
    $schemaReport = Get-Content -LiteralPath $schemaReferences[0].path -Raw -Encoding utf8 | ConvertFrom-Json
    if (!(Test-ValidationReportPassed $schemaReport) -or $schemaReport.helper_sha256 -ne (Digest (Join-Path $PSScriptRoot 'validation-report.ps1'))) { throw 'Receipt schema validation is failed or stale.' }
    foreach ($stage in @('m1','m2','m3','m4','m5-ordinary','m5-administrator','m6','m7','m7-administrator','core','m8','m9','m10','m10-backend','m11','m12','m13-files','m13-files-administrator','m13-settings','settings-entry','http-close','http-boundary')) {
        $references = @($acceptance.reports | Where-Object { $_.stage -eq $stage -and $_.subject -eq 'product' -and $_.configuration -eq 'Release' })
        if ($references.Count -ne 1) { throw "Missing current product acceptance: $stage" }
        $report = Get-Content -LiteralPath $references[0].path -Raw -Encoding utf8 | ConvertFrom-Json
        if (!(Test-ValidationReportPassed $report) -or $references[0].passed -ne $true) { throw "Product acceptance did not pass: $stage" }
        if ($stage -eq 'core' -and $report.scope -ne 'full') { throw 'Candidate requires the core suite with the connection-close probe included.' }
        if ($stage -in @('m5-administrator','m7-administrator','m13-files-administrator') -and $report.uac_attempted -ne $true) { throw 'Current actual administrator evidence is required.' }
        if ($stage -in @('m13-files','m13-files-administrator') -and $report.cross_directory_verified -ne $true) { throw 'Actual native cross-directory moves must pass before candidate packaging.' }
    }
}
$versionText = & $exe --version
if ($LASTEXITCODE -ne 0 -or $versionText -notmatch '^ShellBot ([0-9A-Za-z.\-]+) \(C17\)$') { throw 'Cannot identify product version.' }
$version = $Matches[1]
$kind = if ($Preview) { 'development-preview' } else { 'candidate' }
$name = "ShellBot-$version-windows-x64-$kind-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$stagePath = Join-Path $repoRoot "dist/$name"
$archive = "$stagePath.zip"
if ((Test-Path -LiteralPath $stagePath) -or (Test-Path -LiteralPath $archive)) { throw 'Destination exists; preserve the earlier package.' }
[void][IO.Directory]::CreateDirectory($stagePath)
$files = [ordered]@{}
foreach ($product in $products) { $files[$product] = Join-Path $repoRoot "out/x64/Release/$product" }
$files['README.md'] = Join-Path $repoRoot 'README.md'
foreach ($doc in @($source.files | Where-Object { $_.relative_path -like 'docs/*.md' })) { $files[$doc.relative_path] = Join-Path $repoRoot $doc.relative_path }
foreach ($example in @('manager.example.json','projects.example.json','desktop.example.json','service.example.json')) { $files["config/$example"] = Join-Path $repoRoot "config/$example" }
$files['LICENSE.txt'] = Join-Path $repoRoot 'LICENSE.txt'
$files['licenses/yyjson-MIT.txt'] = Join-Path $repoRoot 'third_party/yyjson/LICENSE'
$files['licenses/CivetWeb-MIT.txt'] = Join-Path $repoRoot 'third_party/civetweb/LICENSE.md'
$files['licenses/PDFium-binaries-MIT.txt'] = Join-Path $repoRoot 'third_party/pdfium/LICENSE'
$files['licenses/PDFium-provenance.json'] = Join-Path $repoRoot 'third_party/pdfium/provenance.json'
foreach ($license in Get-ChildItem -LiteralPath (Join-Path $repoRoot 'third_party/pdfium/licenses') -File) { $files["licenses/pdfium/$($license.Name)"] = $license.FullName }
$files['validation/acceptance.json'] = $AcceptancePath
$files['validation/source-manifest.json'] = $acceptance.source_manifest.path
foreach ($reference in $acceptance.reports) { $files['validation/' + (Split-Path -Leaf $reference.path)] = $reference.path }
foreach ($entry in $files.GetEnumerator()) {
    $target = Join-Path $stagePath $entry.Key
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
    Copy-Item -LiteralPath $entry.Value -Destination $target
}
$utf8 = [Text.UTF8Encoding]::new($false)
$commands = @{
    'Configure-Projects.cmd' = @'
@echo off
if exist "%~dp0projects.local.json" (
  "%~dp0ShellBot-Launcher.exe" --project-settings --projects "%~dp0projects.local.json"
) else (
  "%~dp0ShellBot-Launcher.exe" --project-settings --projects "%~dp0config\manager.example.json"
)
'@
    'Start-Manager.cmd' = @'
@echo off
if not exist "%~dp0projects.local.json" (
  echo Run Configure-Projects.cmd and save projects.local.json here first.
  exit /b 2
)
"%~dp0WinShellBot.exe" --manager --http --projects "%~dp0projects.local.json" --manager-data-dir "%~dp0Data\Manager"
'@
    'Open-Management.cmd' = @'
@echo off
"%~dp0WinShellBot.exe" --manage --manager-data-dir "%~dp0Data\Manager"
'@
}
$lf = [string][char]10
$crlf = [string][char]13 + [string][char]10
foreach ($command in $commands.GetEnumerator()) { [IO.File]::WriteAllText((Join-Path $stagePath $command.Key),$command.Value.Replace($crlf,$lf).Replace($lf,$crlf)+$crlf,$utf8) }
$manifest = [ordered]@{
    version=$version; stage='M13'; package_kind=$kind; acceptance_status=$acceptance.status
    source_commit=$commit; source_local_changes=$acceptance.working_tree_dirty; source_manifest_sha256=$acceptance.source_manifest.sha256
    executable_sha256=$exeHash; acceptance_sha256=(Digest $AcceptancePath); tools=37
    compiler_language='C17'; runtime='static MSVC CRT'; bundled_data=$false; bundled_credentials=$false
    pending=@($acceptance.pending); external_systems='pending'; real_mcp_client='pending'
    connection_close=$acceptance.connection_close; license_status='Full third-party notices included; project copyright placeholder remains before formal release'
    dependencies=@('Windows system DLLs','ShellBot-SandboxProbe.exe','ShellBot-Launcher.exe','pinned x64 pdfium.dll')
}
$manifest['http_tcp_observations'] = @($acceptance.reports | Where-Object { $_.stage -eq 'http-close' -and $_.subject -eq 'product' } | ForEach-Object {
    $tcp = Get-Content -LiteralPath $_.path -Raw -Encoding utf8 | ConvertFrom-Json
    [ordered]@{ configuration=$_.configuration; connect_timeout_seconds=$tcp.connect_timeout_seconds; io_timeout_seconds=$tcp.io_timeout_seconds; max_connect_ms=$tcp.max_connect_ms; slow_connections=@($tcp.slow_connections); groups=@($tcp.groups) }
})
[IO.File]::WriteAllText((Join-Path $stagePath 'manifest.json'),($manifest | ConvertTo-Json -Depth 12),$utf8)
$hashes = @(Get-ChildItem -LiteralPath $stagePath -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path=$_.FullName.Substring($stagePath.Length+1).Replace('\','/'); sha256=(Digest $_.FullName) }
})
if ($hashes.path -match '(^|/)(Data|tests)(/|$)' -or $hashes.path -contains 'projects.local.json' -or $hashes.path -match 'TestFixture|NativeFileHost|SettingsUiHost|HttpReplyTests') { throw 'Package contains runtime data or a development fixture.' }
[IO.File]::WriteAllText((Join-Path $stagePath 'SHA256SUMS.json'),(ConvertTo-Json -InputObject $hashes -Depth 4),$utf8)
Compress-Archive -LiteralPath $stagePath -DestinationPath $archive -CompressionLevel Optimal
$archiveHash = Digest $archive
[IO.File]::WriteAllText("$archive.sha256","$archiveHash  $name.zip"+$lf,$utf8)
$receipt = [ordered]@{ stage='M13'; status='created_pending_extracted_check'; kind=$kind; archive=$archive; archive_sha256=$archiveHash; directory=$stagePath; source_commit=$commit; source_manifest_sha256=$acceptance.source_manifest.sha256; executable_sha256=$exeHash; product_probe_sha256=(Digest (Join-Path $repoRoot 'out/x64/Release/ShellBot-SandboxProbe.exe')); files=$hashes.Count }
[IO.File]::WriteAllText((Join-Path $repoRoot 'build/validation/m13-package-receipt.json'),($receipt | ConvertTo-Json -Depth 8),$utf8)
Write-Output "Package: $archive"
Write-Output "SHA256: $archiveHash"
