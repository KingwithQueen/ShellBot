param([ValidateSet('x64')][string]$Platform = 'x64')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repoRoot "out\$Platform\Release\WinShellBot.exe"
$receiptPath = Join-Path $repoRoot "build\validation\$Platform-Release-core.json"
if (!(Test-Path -LiteralPath $executable) -or !(Test-Path -LiteralPath $receiptPath)) { throw 'Build and validate Release core before packaging.' }
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
$artifactHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($receipt.status -ne 'passed' -or $receipt.artifact.sha256 -ne $artifactHash -or $receipt.scope -ne 'core_except_connection_close') {
    throw 'Core validation receipt must match the current executable and declared testing scope.'
}
$localAcceptance = Join-Path $repoRoot 'build\validation\dev7-local-acceptance.json'
$nativeAcceptance = Get-Content -LiteralPath $localAcceptance -Raw | ConvertFrom-Json
$fixture = Join-Path $repoRoot "out\$Platform\Release\ShellBot-TestFixture.exe"
if ($nativeAcceptance.status -ne 'passed' -or $nativeAcceptance.artifact_sha256 -ne $artifactHash -or $nativeAcceptance.test_fixture_sha256 -ne (Get-FileHash -LiteralPath $fixture).Hash.ToLowerInvariant()) { throw 'Native task acceptance must match both current executables.' }
foreach ($reference in $nativeAcceptance.reports.PSObject.Properties) {
    $testPath = Join-Path $repoRoot $reference.Value.path
    if ($reference.Value.status -ne 'passed' -or (Get-FileHash -LiteralPath $testPath).Hash.ToLowerInvariant() -ne $reference.Value.sha256) { throw 'Original acceptance report changed or did not pass.' }
}
$versionText = & $executable --version
if ($LASTEXITCODE -ne 0 -or $versionText -notmatch '^ShellBot ([0-9A-Za-z.\-]+) \(C17\)$') { throw 'Cannot identify artifact version.' }
$version = $Matches[1]
$packageName = "ShellBot-$version-windows-$Platform-testing-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$distRoot = Join-Path $repoRoot 'dist'
$stage = Join-Path $distRoot $packageName
$archive = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) { throw 'Package destination already exists; existing packages are preserved.' }
[void][IO.Directory]::CreateDirectory($stage)
$utf8 = New-Object Text.UTF8Encoding($false)
$files = [ordered]@{
    'WinShellBot.exe' = $executable
    'Test-ShellBot.ps1' = (Join-Path $repoRoot 'tests\Test-ShellBot.ps1')
    'ShellBot-TestFixture.exe' = $fixture
    'ShellBot-TestFixture.c' = (Join-Path $repoRoot 'tests\task_native_fixture.c')
    'Test-ShellBot-Tasks.ps1' = (Join-Path $repoRoot 'tests\Test-ShellBot-Tasks.ps1')
    'COMMAND_TASKS.md' = (Join-Path $repoRoot 'docs\COMMAND_TASKS.md')
    'validation\local-acceptance.json' = $localAcceptance
    'validation\development-user-tasks.json' = (Join-Path $repoRoot $nativeAcceptance.reports.native_user_tasks.path)
    'validation\development-administrator-tasks.json' = (Join-Path $repoRoot $nativeAcceptance.reports.native_administrator_tasks.path)
    'Test-ShellBot-Permissions.ps1' = (Join-Path $repoRoot 'tests\Test-ShellBot-Permissions.ps1')
    'TESTING.md' = (Join-Path $repoRoot 'docs\SERVER_2025_TEST.md')
    'AUDIT.md' = (Join-Path $repoRoot 'docs\AUDIT.md')
    'EXTERNAL_ACCESS.md' = (Join-Path $repoRoot 'docs\EXTERNAL_ACCESS.md')
    'CONTENT_SEARCH.md' = (Join-Path $repoRoot 'docs\CONTENT_SEARCH.md')
    'FILE_EDIT.md' = (Join-Path $repoRoot 'docs\FILE_EDIT.md')
    'EXECUTION_PERMISSIONS.md' = (Join-Path $repoRoot 'docs\EXECUTION_PERMISSIONS.md')
    'shellbot.example.json' = (Join-Path $repoRoot 'config\shellbot.example.json')
    'LICENSE.txt' = (Join-Path $repoRoot 'LICENSE.txt')
    'licenses\yyjson-MIT.txt' = (Join-Path $repoRoot 'third_party\yyjson\LICENSE')
    'licenses\CivetWeb-MIT.txt' = (Join-Path $repoRoot 'third_party\civetweb\LICENSE.md')
    'validation\development-core.json' = $receiptPath
}
foreach ($file in $files.GetEnumerator()) {
    $destination = Join-Path $stage $file.Key
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
    Copy-Item -LiteralPath $file.Value -Destination $destination
}
$sourceFiles = & git -C $repoRoot -c core.quotepath=false ls-files --cached --others --exclude-standard
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source files.' }
$sourceManifest = @($sourceFiles | Sort-Object -Unique | ForEach-Object {
    $sourcePath = Join-Path $repoRoot $_
    if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
        [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
})
$sourceJson = ConvertTo-Json -InputObject $sourceManifest -Depth 5 -Compress
$sha = [Security.Cryptography.SHA256]::Create()
try { $sourceHash = [BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($sourceJson))).Replace('-', '').ToLowerInvariant() } finally { $sha.Dispose() }
$commit = & git -C $repoRoot rev-parse HEAD
$branch = & git -C $repoRoot branch --show-current
$changes = @(& git -C $repoRoot status --porcelain)
$manifest = [ordered]@{
    version = $version; purpose = 'Windows native runtime testing; not a completed phase or public release'
    target = 'Windows 10/11 x64; independent Windows Server 2025 acceptance pending'
    built_at_utc = [DateTime]::UtcNow.ToString('o'); compiler_language = 'C17'; runtime = 'static MSVC CRT'
    source = [ordered]@{ commit = $commit; branch = $branch; local_changes = ($changes.Count -gt 0); snapshot_sha256 = $sourceHash; files = $sourceManifest }
    test_fixture = [ordered]@{ purpose = 'fixed-operation native test child; not required for product runtime'; sha256 = $nativeAcceptance.test_fixture_sha256; source_sha256 = $nativeAcceptance.test_fixture_source_sha256 }
    executable_sha256 = $artifactHash
    validation = [ordered]@{ core = 'passed on development machine'; assertions = $receipt.assertions_passed; skipped = $receipt.skipped; execution = 'native user/administrator command tasks passed locally; controlled isolation, rules and remote approval unavailable'; administrator_agent = 'Windows 11 local actual elevated tasks, agent reuse and cleanup passed; one UAC and no new AV alerts observed by user; dev.7 Server 2025 and cross-account pending'; connection_close = 'known failure under investigation; test on Server 2025'; public_mcp = 'delegated to existing user environment; no domain test required'; oauth = 'deferred by user'; tunnel_management = 'external environment assumption' }
    license_status = 'Third-party notices included; project copyright template needs owner completion before formal release'
    dependencies = @(@{ name = 'yyjson'; version = '0.13.0' }, @{ name = 'CivetWeb'; version = '1.16' })
}
[IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 10), $utf8)
$fileHashes = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($stage.Length + 1).Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.json'), (ConvertTo-Json -InputObject $fileHashes -Depth 5), $utf8)
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archive.sha256", ($archiveHash + '  ' + (Split-Path -Leaf $archive) + "`n"), $utf8)
Write-Output "Testing package: $archive"
Write-Output "SHA256: $archiveHash"
