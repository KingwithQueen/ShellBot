$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repoRoot 'out\x64\Release\WinShellBot.exe'
$receiptPath = Join-Path $repoRoot 'build\validation\x64-Release-core.json'
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
$exeHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($receipt.status -ne 'passed' -or $receipt.artifact.sha256 -ne $exeHash) { throw 'Executable must match its core validation receipt.' }
$versionText = & $executable --version
if ($LASTEXITCODE -ne 0 -or $versionText -notmatch '^ShellBot ([0-9A-Za-z.\-]+) \(C17\)$') { throw 'Cannot identify artifact version.' }
$version = $Matches[1]
$name = "ShellBot-$version-endpoint-check-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$stage = Join-Path (Join-Path $repoRoot 'dist') $name
$archive = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) { throw 'Package destination already exists.' }
[void][IO.Directory]::CreateDirectory($stage)
Copy-Item -LiteralPath (Join-Path $repoRoot 'tests\Test-ShellBot-Endpoint.ps1') -Destination (Join-Path $stage 'Test-ShellBot-Endpoint.ps1')
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\EXTERNAL_ACCESS.md') -Destination (Join-Path $stage 'ENDPOINT_TESTING.md')
$utf8 = New-Object Text.UTF8Encoding($false)
$manifest = [ordered]@{
    purpose = 'Read-only validation of an existing ShellBot endpoint; no binary or tunnel changes'
    validated_host_version = $version; baseline_executable_sha256 = $exeHash
    source_commit = (& git -C $repoRoot rev-parse HEAD)
    source_branch = (& git -C $repoRoot branch --show-current)
    local_changes = @(& git -C $repoRoot status --porcelain).Count -gt 0
    report_schema = 1; token_source = 'process environment variable; never a command argument'
    acceptance_scope = 'health, bearer rejection, MCP initialization and read-only host tools; actual model client pending'
}
[IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), (ConvertTo-Json -InputObject $manifest -Depth 6), $utf8)
$hashes = @(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ path = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.json'), (ConvertTo-Json -InputObject $hashes -Depth 5), $utf8)
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archive.sha256", $hash + '  ' + (Split-Path -Leaf $archive) + "`n", $utf8)
Write-Output "Endpoint check package: $archive"
Write-Output "SHA256: $hash"
