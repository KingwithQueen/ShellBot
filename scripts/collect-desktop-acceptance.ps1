param([string]$OutputPath = '')
$ErrorActionPreference='Stop'
$desktopRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
function DesktopDigest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$desktopReports=Join-Path $desktopRoot 'build/desktop-validation'
if (!$OutputPath) { $OutputPath=Join-Path $desktopReports 'acceptance.json' }
$desktopArtifacts=[ordered]@{}
foreach ($configuration in @('Debug','Release')) {
    $items=[ordered]@{}
    foreach ($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')) {
        $path=Join-Path $desktopRoot "out/x64/$configuration/$name"
        $items[$name]=@{path=$path;sha256=(DesktopDigest $path)}
    }
    $desktopArtifacts[$configuration]=$items
}
$desktopReferences=@();$desktopPending=@()
$desktopStages=@('m1','m2','m3','m4','m5-ordinary','m6','m7','core','m9','m10','m10-backend','m11','m12','m13-files','m13-settings','http-boundary')
foreach ($stage in $desktopStages) {
    $path=Join-Path $desktopReports "m14-release-$stage.json"
    if (!(Test-Path -LiteralPath $path)) { $desktopPending+="Missing Release $stage";continue }
    $value=Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
    $hash=if ($value.sha256) {$value.sha256} elseif ($value.exe_sha256) {$value.exe_sha256} else {$value.artifact.sha256}
    $passed=(Test-ValidationReportPassed $value) -and $hash -eq $desktopArtifacts.Release['WinShellBot.exe'].sha256
    if ($stage -eq 'core' -and $value.scope -ne 'full') {$passed=$false}
    if ($stage -eq 'm13-files' -and $value.cross_directory_verified -ne $true) {$passed=$false}
    if (!$passed) {$desktopPending+="Failed or stale Release $stage"}
    $count=if ($value.assertions_passed) {$value.assertions_passed} elseif ($value.assertions) {$value.assertions} else {@($value.checks).Count}
    $desktopReferences+=@{stage=$stage;configuration='Release';path=$path;sha256=(DesktopDigest $path);passed=[bool]$passed;assertions=$count}
}
foreach ($configuration in @('Debug','Release')) {
    foreach ($stage in @('app','entry','exit')) {
        $path=Join-Path $desktopRoot "build/desktop-$stage-$($configuration.ToLowerInvariant())-final.json"
        if (!(Test-Path -LiteralPath $path)) {$desktopPending+="Missing $configuration desktop $stage";continue}
        $value=Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
        $passed=(Test-ValidationReportPassed $value) -and $value.sha256 -eq $desktopArtifacts[$configuration]['WinShellBot.exe'].sha256
        if ($stage -eq 'entry' -and $value.entry_sha256 -ne $desktopArtifacts[$configuration]['ShellBot.exe'].sha256) {$passed=$false}
        if (!$passed) {$desktopPending+="Failed or stale $configuration desktop $stage"}
        $desktopReferences+=@{stage="desktop-$stage";configuration=$configuration;path=$path;sha256=(DesktopDigest $path);passed=[bool]$passed;assertions=$value.assertions}
    }
}
$tcpPath=Join-Path $desktopReports 'm14-release-http-close.json'
if (Test-Path -LiteralPath $tcpPath) {
    $tcp=Get-Content -LiteralPath $tcpPath -Raw -Encoding utf8 | ConvertFrom-Json
    $passed=(Test-ValidationReportPassed $tcp) -and $tcp.exe_sha256 -eq $desktopArtifacts.Release['WinShellBot.exe'].sha256
    $groups=@('fresh_keep_alive','close','split_close','rejected_close','close_concurrent')
    if (@($tcp.groups).Count -ne $groups.Count -or $tcp.clean_exit -ne $true -or @($tcp.failures).Count) {$passed=$false}
    foreach($group in $groups) {
        $rows=@($tcp.groups | Where-Object name -eq $group)
        if($rows.Count -ne 1 -or $rows[0].calls -ne 1000 -or $rows[0].failures -ne 0) {$passed=$false}
    }
    if (!$passed) {$desktopPending+='Failed or stale Release HTTP TCP'}
    $desktopReferences+=@{stage='http-close';configuration='Release';path=$tcpPath;sha256=(DesktopDigest $tcpPath);passed=[bool]$passed;groups=$tcp.groups}
} else {$desktopPending+='Missing Release HTTP TCP'}
$desktopSources=@(& git -C $desktopRoot -c core.quotepath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)
if ($LASTEXITCODE) {throw 'Cannot inventory desktop sources.'}
$desktopRows=@($desktopSources | ForEach-Object {$path=Join-Path $desktopRoot $_;@{relative_path=$_;sha256=(DesktopDigest $path);bytes=(Get-Item -LiteralPath $path).Length}})
$desktopUtf8=[Text.UTF8Encoding]::new($false)
$desktopManifest=Join-Path $desktopReports 'source-manifest.json'
[IO.File]::WriteAllText($desktopManifest,(@{files=$desktopRows} | ConvertTo-Json -Depth 6),$desktopUtf8)
$desktopTestRows=@(Get-ChildItem -LiteralPath (Join-Path $desktopRoot 'tests') -File | Where-Object {$_.Extension -in @('.py','.c','.h','.vcxproj')} | ForEach-Object {@{name=$_.Name;sha256=(DesktopDigest $_.FullName)}})
[IO.File]::WriteAllText((Join-Path $desktopReports 'local-test-manifest.json'),(@{files=$desktopTestRows} | ConvertTo-Json -Depth 5),$desktopUtf8)
$desktopAcceptance=[ordered]@{
    stage='M14';status=$(if (!$desktopPending.Count) {'desktop_local_verified'} else {'desktop_validation_pending'})
    version='0.1.0-dev.21';created_utc=[DateTime]::UtcNow.ToString('o');source_commit=(& git -C $desktopRoot rev-parse HEAD).Trim()
    working_tree_dirty=[bool](& git -C $desktopRoot status --porcelain);pushed=$false
    artifacts=$desktopArtifacts;source_manifest=@{path=$desktopManifest;sha256=(DesktopDigest $desktopManifest)}
    reports=$desktopReferences;pending=$desktopPending
    lifecycle='GUI owns manager and all project workers; tray exit stops all projects'
    default_entry='ShellBot.exe';control_cmd_scripts=$false;credentials='per-project user DPAPI; GUI generates tokens'
    real_mcp_client='pending';external_systems='pending';fresh_administrator_uac='pending';physical_mouse_tray_acceptance='pending'
    http_eof_stability='pending';known_limitations='Full ordinary regression and 5000 response calls passed; repeated response-to-EOF boundaries also failed on this machine and on an independent Python reference. Cause is unconfirmed; experimental HTTP changes were reverted.'
    observations=@(@('preserved-extracted-runtime-http-eof-failure.json','preserved-http-eof-before.json','http-eof-reference.json') | ForEach-Object {$path=Join-Path $desktopReports $_;@{name=$_;path=$path;sha256=(DesktopDigest $path)}})
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath),($desktopAcceptance | ConvertTo-Json -Depth 12),$desktopUtf8)
Write-Output "Receipt: $OutputPath"
Write-Output "Status: $($desktopAcceptance.status); pending: $($desktopPending.Count)"
