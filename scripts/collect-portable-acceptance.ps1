param([string]$OutputPath = '')
$ErrorActionPreference='Stop'
$desktopRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
function DesktopDigest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$desktopReports=Join-Path $desktopRoot 'build/portable-validation'
if (!$OutputPath) { $OutputPath=Join-Path $desktopReports 'acceptance.json' }
$desktopArtifacts=[ordered]@{}
foreach ($configuration in @('Debug','Release')) {
    $items=[ordered]@{}
    foreach ($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll','ShellBot-TestFixture.exe','ShellBot-ProjectPolicyTests.exe')) {
        $path=Join-Path $desktopRoot "out/x64/$configuration/$name"
        $items[$name]=@{path=$path;sha256=(DesktopDigest $path)}
    }
    $desktopArtifacts[$configuration]=$items
}
$desktopReferences=@();$desktopPending=@()
$desktopStages=@('m1','m2','m3','m4','m5-ordinary','m6','m7','core','m9','m10','m10-backend','m11','m12','m13-files','m13-settings','http-response-boundary')
foreach ($stage in $desktopStages) {
    $path=Join-Path $desktopReports "m16-release-$stage.json"
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
$oauthMatrix=@(
    @('Debug','portable','portable-debug.json'),@('Release','portable','portable-release.json'),
    @('Debug','oauth','oauth-debug.json'),@('Release','oauth','oauth-release.json'),
    @('Release','oauth-ui','oauth-ui-release.json'),
    @('Debug','manager-refresh','manager-refresh-debug.json'),@('Release','manager-refresh','manager-refresh-release.json'),
    @('Release','desktop-app','desktop-app-release.json'),@('Release','desktop-entry','desktop-entry-release.json'),@('Release','desktop-exit','desktop-exit-release.json')
)
foreach ($row in $oauthMatrix) {
    $configuration=$row[0];$stage=$row[1];$path=Join-Path $desktopReports $row[2]
    if (!(Test-Path -LiteralPath $path)) {$desktopPending+="Missing $configuration $stage";continue}
    $value=Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
    $passed=(Test-ValidationReportPassed $value) -and $value.sha256 -eq $desktopArtifacts[$configuration]['WinShellBot.exe'].sha256
    if ($stage -eq 'desktop-entry' -and $value.entry_sha256 -ne $desktopArtifacts[$configuration]['ShellBot.exe'].sha256) {$passed=$false}
    if ($stage -eq 'oauth' -and $configuration -eq 'Release' -and $value.real_code_expiry_verified -cne $true) {$passed=$false}
    if (@($value.checks | Where-Object {$_.passed -isnot [bool] -or $_.passed -cne $true}).Count) {$passed=$false}
    if (!$passed) {$desktopPending+="Failed or stale $configuration $stage"}
    $desktopReferences+=@{stage=$stage;configuration=$configuration;path=$path;sha256=(DesktopDigest $path);passed=[bool]$passed;assertions=$value.assertions}
}
# OAuth interoperability evidence is separate from the unchanged candidate gates.
# Failed network observations stay failed and are bundled for real-client testing.
$tcpPath=Join-Path $desktopReports 'm16-release-http-close.json'
$oauthNetwork=$null
if (Test-Path -LiteralPath $tcpPath) {
    $tcp=Get-Content -LiteralPath $tcpPath -Raw -Encoding utf8 | ConvertFrom-Json
    $complete=$tcp.exe_sha256 -eq $desktopArtifacts.Release['WinShellBot.exe'].sha256 -and $tcp.passed -is [bool] -and $tcp.clean_exit -ceq $true
    $groups=@('fresh_keep_alive','close','split_close','rejected_close','close_concurrent')
    if (@($tcp.groups).Count -ne $groups.Count) {$complete=$false}
    foreach ($group in $groups) {
        $rows=@($tcp.groups | Where-Object name -eq $group)
        if ($rows.Count -ne 1 -or $rows[0].calls -ne 200 -or $rows[0].failures -lt 0) {$complete=$false}
    }
    if (!$complete) {$desktopPending+='Missing or stale complete network observation'}
    $oauthNetwork=@{path=$tcpPath;sha256=(DesktopDigest $tcpPath);complete=[bool]$complete;passed=$tcp.passed;failures=@($tcp.failures).Count;calls=1000}
} else {$desktopPending+='Missing Release HTTP TCP observation'}
$desktopSources=@(& git -C $desktopRoot -c core.quotepath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)
if ($LASTEXITCODE) {throw 'Cannot inventory desktop sources.'}
$desktopRows=@($desktopSources | ForEach-Object {$path=Join-Path $desktopRoot $_;@{relative_path=$_;sha256=(DesktopDigest $path);bytes=(Get-Item -LiteralPath $path).Length}})
$desktopUtf8=[Text.UTF8Encoding]::new($false)
$desktopManifest=Join-Path $desktopReports 'source-manifest.json'
[IO.File]::WriteAllText($desktopManifest,(@{files=$desktopRows} | ConvertTo-Json -Depth 6),$desktopUtf8)
$desktopTestRows=@(Get-ChildItem -LiteralPath (Join-Path $desktopRoot 'tests') -File | Where-Object {$_.Extension -in @('.py','.c','.h','.vcxproj')} | ForEach-Object {@{name=$_.Name;sha256=(DesktopDigest $_.FullName)}})
$oauthTestManifest=Join-Path $desktopReports 'local-test-manifest.json'
[IO.File]::WriteAllText($oauthTestManifest,(@{files=$desktopTestRows} | ConvertTo-Json -Depth 5),$desktopUtf8)
$desktopAcceptance=[ordered]@{
    stage='M16';status=$(if (!$desktopPending.Count) {'portable_functional_verified'} else {'portable_validation_pending'})
    version='0.1.0-dev.23';created_utc=[DateTime]::UtcNow.ToString('o');source_commit=(& git -C $desktopRoot rev-parse HEAD).Trim()
    working_tree_dirty=[bool](& git -C $desktopRoot status --porcelain);pushed=$false
    artifacts=$desktopArtifacts;source_manifest=@{path=$desktopManifest;sha256=(DesktopDigest $desktopManifest)}
    local_test_manifest=@{path=$oauthTestManifest;sha256=(DesktopDigest $oauthTestManifest)}
    network_boundary=@{path=(Join-Path $desktopReports 'm16-release-http-boundary.json');sha256=(DesktopDigest (Join-Path $desktopReports 'm16-release-http-boundary.json'));passed=$false}
    network_observation=$oauthNetwork
    reports=$desktopReferences;pending=$desktopPending
    oauth=@{authorization='code + S256 PKCE';client_registration='dynamic public clients';confirmation='local Windows owner GUI';access_seconds=900;refresh_max_days=30;storage='current-user DPAPI'}
    lifecycle='GUI owns manager and all project workers; tray exit stops all projects'
    default_entry='ShellBot.exe';control_cmd_scripts=$false;credentials='per-project user DPAPI; GUI generates tokens'
    real_mcp_client='pending';external_systems='pending';fresh_administrator_uac='pending';physical_mouse_tray_acceptance='pending'
    http_eof_stability='failed';http_response_stress=$(if ($oauthNetwork.passed -ceq $true) {'passed'} else {'failed'});known_limitations="Current response stress: $($oauthNetwork.failures)/$($oauthNetwork.calls) failed. Historical dev.22 response stress had 3 connect timeouts in 5000 calls; baseline and Python references had differing network failures. Current EOF boundary also failed with a connection reset; historical TCP EOF stability and real external clients remain unresolved; this portable test package is not a fully accepted release."
    portable=@{default_data='program_directory/Data';location_setting='program_directory/ShellBot.storage.json';migration='copy, SHA-256 verify, atomic location publication, restart, manual old-directory cleanup';c_drive_application_storage=$false;authorized_c_drive_file_operations=$true;sandbox_disk_profile=$false}

    observations=@(@('preserved-extracted-runtime-http-eof-failure.json','preserved-http-eof-before.json','http-eof-reference.json') | ForEach-Object {$path=Join-Path $desktopRoot "build/desktop-validation/$_";@{name=$_;path=$path;sha256=(DesktopDigest $path)}}) + @(@('preserved-http-connect-failure.json','http-connect-baseline16.json','http-connect-reference16.json') | ForEach-Object {$path=Join-Path $desktopRoot "build/oauth-validation/$_";@{name=$_;path=$path;sha256=(DesktopDigest $path)}})
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath),($desktopAcceptance | ConvertTo-Json -Depth 12),$desktopUtf8)
Write-Output "Receipt: $OutputPath"
Write-Output "Status: $($desktopAcceptance.status); pending: $($desktopPending.Count)"
