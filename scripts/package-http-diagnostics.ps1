$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$receiptPath = Join-Path $repoRoot 'build\validation\x64-Release-core.json'
$executable = Join-Path $repoRoot 'out\x64\Release\WinShellBot.exe'
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
$exeHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($receipt.status -ne 'passed' -or $receipt.artifact.sha256 -ne $exeHash) { throw 'The baseline executable must match its core validation receipt.' }
$versionText = & $executable --version
if ($LASTEXITCODE -ne 0 -or $versionText -notmatch '^ShellBot ([0-9A-Za-z.\-]+) \(C17\)$') { throw 'Cannot identify artifact version.' }
$version = $Matches[1]
$name = "ShellBot-$version-http-diagnostics-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$stage = Join-Path (Join-Path $repoRoot 'dist') $name
$archive = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) { throw 'Preserving the existing diagnostic package; choose a new destination.' }
[void][IO.Directory]::CreateDirectory($stage)
Copy-Item -LiteralPath (Join-Path $repoRoot 'tests\Test-ShellBot.ps1') -Destination (Join-Path $stage 'Test-ShellBot-Http.ps1')
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\HTTP_DIAGNOSTICS.md') -Destination (Join-Path $stage 'HTTP-DIAGNOSTICS.md')
$utf8 = New-Object Text.UTF8Encoding($false)
$manifest = [ordered]@{
    purpose = 'Diagnostic script for the matching executable version; no binary replacement'
    supported_version = $version; baseline_executable_sha256 = $exeHash
    source_commit = (& git -C $repoRoot rev-parse HEAD)
    source_branch = (& git -C $repoRoot branch --show-current)
    local_changes = @(& git -C $repoRoot status --porcelain).Count -gt 0
    report_schema = 2; recommended_arguments = '-HttpOnly'
    diagnostic_status = 'Connection-close failure under investigation'
}
[IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 6), $utf8)
$hashes = @(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ path = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.json'), (ConvertTo-Json -InputObject $hashes -Depth 5), $utf8)
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archive.sha256", $hash + '  ' + (Split-Path -Leaf $archive) + "`n", $utf8)
Write-Output "Diagnostic package: $archive"
Write-Output "SHA256: $hash"
