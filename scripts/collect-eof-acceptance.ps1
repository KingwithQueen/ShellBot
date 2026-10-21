param([string]$OutputPath = '',[ValidateSet('M17','M18','M19')][string]$Milestone='M17')
$ErrorActionPreference='Stop'
$desktopRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
function DesktopDigest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$desktopReports=Join-Path $desktopRoot $(if($Milestone -eq 'M19'){'build/persistent-auth-validation'}elseif($Milestone -eq 'M18'){'build/auth-refresh-validation'}else{'build/eof-validation'})
$desktopPrefix=$Milestone.ToLowerInvariant()
$desktopVersion=if($Milestone -eq 'M19'){'0.1.0-dev.26'}elseif($Milestone -eq 'M18'){'0.1.0-dev.25'}else{'0.1.0-dev.24'}
if (!$OutputPath) { $OutputPath=Join-Path $desktopReports 'acceptance.json' }
$desktopArtifacts=[ordered]@{}
foreach ($configuration in @('Debug','Release')) {
    $items=[ordered]@{}
    foreach ($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll','ShellBot-TestFixture.exe','ShellBot-ProjectPolicyTests.exe','ShellBot-HttpReplyTests.exe')) {
        $path=Join-Path $desktopRoot "out/x64/$configuration/$name"
        $items[$name]=@{path=$path;sha256=(DesktopDigest $path)}
    }
    $desktopArtifacts[$configuration]=$items
}
$desktopReferences=@();$desktopPending=@()
$desktopStages=@('m1','m2','m3','m4','m5-ordinary','m6','m7','core','m9','m10','m10-backend','m11','m12','m13-files','m13-settings','http-boundary')
foreach ($stage in $desktopStages) {
    $path=Join-Path $desktopReports "$desktopPrefix-release-$stage.json"
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
    @('Debug','http-reply','http-reply-debug.json'),@('Release','http-reply','http-reply-release.json'),
    @('Debug','http-shutdown','http-shutdown-debug.json'),@('Release','http-shutdown','http-shutdown-release.json'),
    @('Debug','http-eof','http-eof-debug.json'),@('Release','http-eof','http-eof-release.json'),
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
    if ($value.checks -and @($value.checks | Where-Object {$_.passed -isnot [bool] -or $_.passed -cne $true}).Count) {$passed=$false}
    if ($stage -eq 'http-reply' -and ($value.stage -ne 'HTTP-reply-unit' -or $value.assertions -lt 28 -or $value.unit_sha256 -ne $desktopArtifacts[$configuration]['ShellBot-HttpReplyTests.exe'].sha256 -or $value.header_sha256 -ne (DesktopDigest (Join-Path $desktopRoot 'WinShellBot/app/http_response.h')))) {$passed=$false}
    if ($stage -eq 'http-shutdown' -and ($value.stage -ne 'HTTP-shutdown' -or $value.assertions -lt 26)) {$passed=$false}
    if ($stage -eq 'http-eof') {
        $expected=if($configuration -eq 'Debug'){100}else{1000}
        if($value.stage -ne 'HTTP-EOF' -or $value.clean_exit -cne $true -or @($value.failures).Count -or @($value.groups).Count -ne 4){$passed=$false}
        foreach($group in @('oversize-no-body','oversize-half-close','incomplete-half-close','normal-close')) {
            $rows=@($value.groups | Where-Object name -eq $group)
            if($rows.Count -ne 1 -or $rows[0].calls -ne $expected -or $rows[0].failures -ne 0){$passed=$false}
        }
    }
    if (!$passed) {$desktopPending+="Failed or stale $configuration $stage"}
    $desktopReferences+=@{stage=$stage;configuration=$configuration;path=$path;sha256=(DesktopDigest $path);passed=[bool]$passed;assertions=$(if($stage -eq 'http-eof'){($value.groups | Measure-Object calls -Sum).Sum}else{$value.assertions})}
}
# OAuth interoperability evidence is separate from the unchanged candidate gates.
# Failed network observations stay failed and are bundled for real-client testing.
$tcpPath=Join-Path $desktopReports "$desktopPrefix-release-http-close.json"
$oauthNetwork=$null
if (Test-Path -LiteralPath $tcpPath) {
    $tcp=Get-Content -LiteralPath $tcpPath -Raw -Encoding utf8 | ConvertFrom-Json
    $complete=$tcp.exe_sha256 -eq $desktopArtifacts.Release['WinShellBot.exe'].sha256 -and $tcp.passed -is [bool] -and $tcp.clean_exit -ceq $true
    $groups=@('fresh_keep_alive','close','split_close','rejected_close','close_concurrent')
    if (@($tcp.groups).Count -ne $groups.Count) {$complete=$false}
    foreach ($group in $groups) {
        $rows=@($tcp.groups | Where-Object name -eq $group)
        if ($rows.Count -ne 1 -or $rows[0].calls -ne 1000 -or $rows[0].failures -lt 0) {$complete=$false}
    }
    if($tcp.passed -cne $true -or @($tcp.failures).Count){$complete=$false};if (!$complete) {$desktopPending+='Missing, failed or stale complete network observation'}
    $oauthNetwork=@{path=$tcpPath;sha256=(DesktopDigest $tcpPath);complete=[bool]$complete;passed=$tcp.passed;failures=@($tcp.failures).Count;calls=5000}
} else {$desktopPending+='Missing Release HTTP TCP observation'}
$eofBoundaryPath=Join-Path $desktopReports "$desktopPrefix-release-http-boundary.json"
$eofBoundaryValue=if(Test-Path -LiteralPath $eofBoundaryPath){Get-Content -LiteralPath $eofBoundaryPath -Raw -Encoding utf8 | ConvertFrom-Json}else{$null}
$eofBoundaryPassed=$eofBoundaryValue -and $eofBoundaryValue.passed -ceq $true -and $eofBoundaryValue.scope -eq 'full-with-EOF' -and $eofBoundaryValue.sha256 -eq $desktopArtifacts.Release['WinShellBot.exe'].sha256
if(!$eofBoundaryPassed){$desktopPending+='Missing or failed full EOF boundary'}
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
    stage=$Milestone;status=$(if (!$desktopPending.Count) {'full_http_regression_verified'} else {'http_regression_pending'})
    version=$desktopVersion;created_utc=[DateTime]::UtcNow.ToString('o');source_commit=(& git -C $desktopRoot rev-parse HEAD).Trim()
    working_tree_dirty=[bool](& git -C $desktopRoot status --porcelain);pushed=$false
    artifacts=$desktopArtifacts;source_manifest=@{path=$desktopManifest;sha256=(DesktopDigest $desktopManifest)}
    local_test_manifest=@{path=$oauthTestManifest;sha256=(DesktopDigest $oauthTestManifest)}
    network_boundary=@{path=$eofBoundaryPath;sha256=$(if($eofBoundaryValue){DesktopDigest $eofBoundaryPath}else{''});passed=[bool]$eofBoundaryPassed}
    network_observation=$oauthNetwork
    reports=$desktopReferences;pending=$desktopPending
    oauth=@{authorization='code + S256 PKCE';client_registration='dynamic public clients';confirmation='local Windows owner GUI';access_seconds=900;refresh_max_days=30;storage='current-user DPAPI'}
    lifecycle='GUI owns manager and all project workers; tray exit stops all projects'
    default_entry='ShellBot.exe';control_cmd_scripts=$false;credentials='per-project user DPAPI; GUI generates tokens'
    real_mcp_client='pending';external_systems='pending';fresh_administrator_uac='pending';physical_mouse_tray_acceptance='pending'
    http_eof_stability=$(if(!$desktopPending.Count){'passed'}else{'pending'});http_response_stress=$(if ($oauthNetwork.passed -ceq $true) {'passed'} else {'failed'});known_limitations='Local automated regression and external client acceptance are separate. Historical dev.23 EOF/connection failures are preserved; current results bind exact binaries. Third-party loopback interception must be documented separately; real MCP clients and fresh ShellBot administrator acceptance remain pending.'
    environment_receipt=@{path=(Join-Path $desktopReports 'environment.json');sha256=(DesktopDigest (Join-Path $desktopReports 'environment.json'))}
    environment=@{adguard_service='Running';localhost_filter='disabled manually by user';causal_control='on-off-on independent native TCP probe; stopped for 11 seconds and restored';local_proxy_filtering='may be affected; per-application exclusions require separate verification'}
    portable=@{default_data='program_directory/Data';location_setting='program_directory/ShellBot.storage.json';migration='copy, SHA-256 verify, atomic location publication, restart, manual old-directory cleanup';c_drive_application_storage=$false;authorized_c_drive_file_operations=$true;sandbox_disk_profile=$false}

    observations=@(@('preserved-extracted-runtime-http-eof-failure.json','preserved-http-eof-before.json','http-eof-reference.json') | ForEach-Object {$path=Join-Path $desktopRoot "build/desktop-validation/$_";@{name=$_;path=$path;sha256=(DesktopDigest $path)}}) + @(@('preserved-http-connect-failure.json','http-connect-baseline16.json','http-connect-reference16.json') | ForEach-Object {$path=Join-Path $desktopRoot "build/oauth-validation/$_";@{name=$_;path=$path;sha256=(DesktopDigest $path)}})
}
$eofObservationFiles=@('dev23-before.json','dev24-before-adguard-control.json','adguard-control.json','dev23-adguard-stopped.json','dev24-adguard-stopped.json','dev23-early-reject-before.json','native-probe.log','native-probe-adguard-stopped.log','native-probe-adguard-restored.log','native-probe-after-user-localhost-setting.log')
$desktopAcceptance.observations+=@($eofObservationFiles | ForEach-Object {$path=Join-Path $desktopRoot "build/eof-validation/$_";@{name=$_;path=$path;sha256=(DesktopDigest $path)}})
if($Milestone -in @('M18','M19')){
    $refreshReports=Join-Path $desktopRoot 'build/auth-refresh-validation'
    $desktopAcceptance.observations+=@(@('dev24-ui-before.json','dev24-oauth-before.json')|ForEach-Object{$path=Join-Path $refreshReports $_;@{name=$_;path=$path;sha256=(DesktopDigest $path)}})
    $desktopAcceptance.observations+=@(@{name='real-client-dev24.json';path=(Join-Path $desktopRoot 'build/real-client-acceptance-20261021/session-mvhmh459.json');sha256=(DesktopDigest (Join-Path $desktopRoot 'build/real-client-acceptance-20261021/session-mvhmh459.json'))})
    $desktopAcceptance.observations+=@(@('http-eof-debug.json','native-probe-current.log')|ForEach-Object{$path=Join-Path $refreshReports "preserved-before-app-exclusions/$_";@{name="before-app-exclusions-$_";path=$path;sha256=(DesktopDigest $path)}})
    $desktopAcceptance.observations+=@(@{name='initial-global-navigation-failure.json';path=(Join-Path $refreshReports 'preserved-initial-navigation/oauth-debug.json');sha256=(DesktopDigest (Join-Path $refreshReports 'preserved-initial-navigation/oauth-debug.json'))})
    $desktopAcceptance.observations+=@(@('extracted-runtime.json','portable-failure.json')|ForEach-Object{$path=Join-Path $refreshReports "preserved-first-extraction/$_";@{name="first-extraction-$_";path=$path;sha256=(DesktopDigest $path)}})
    $desktopAcceptance.observations+=@(@('extracted-runtime.json','http-boundary-failure.json')|ForEach-Object{$path=Join-Path $refreshReports "preserved-unexcluded-extraction/$_";@{name="unexcluded-extraction-$_";path=$path;sha256=(DesktopDigest $path)}})
    $desktopAcceptance.observations+=@(@{name='fixed-path-boundary-control.json';path=(Join-Path $refreshReports 'path-control-fixed-boundary.json');sha256=(DesktopDigest (Join-Path $refreshReports 'path-control-fixed-boundary.json'))})
    $desktopAcceptance.oauth['access_rotation']='unexpired access leases retain original expiry; refresh replay still revokes family'
    $desktopAcceptance.oauth['storage_format']=2
    $desktopAcceptance['real_client_known_issue']='dev.24 real plugin run recorded concurrent errors and issuer mismatch; not yet retested with dev.25'
    $mode=Get-Content -LiteralPath (Join-Path $desktopReports 'environment.json') -Raw -Encoding utf8|ConvertFrom-Json
    $desktopAcceptance.environment=@{adguard_service=$mode.adguard_service;localhost_filter=$mode.localhost_filter;network_mode=$mode.network_mode;app_exclusions=$mode.excluded_apps;local_proxy_filtering='global localhost filtering retained; browser and proxy exclusions unchanged; actual proxy filtering not independently tested'}
}
if($Milestone -eq 'M19'){
    $desktopAcceptance.oauth['storage_format']=3
    $desktopAcceptance.oauth['refresh_max_days']='30 for finite grants; no expiry for explicit long-term owner consent'
    $desktopAcceptance.oauth['long_term_authorization']='explicit owner GUI consent; survives restart and policy changes; current operation policy enforced'
    $desktopAcceptance.oauth['identity_binding']='project credential SHA-256; reused project ID cannot inherit a durable grant'
    $desktopAcceptance.oauth['refresh_scope']='sets accept either order; access scope can narrow while the rotated refresh scope retains the original grant; no widening'
    $desktopAcceptance.oauth['diagnostics']='last token endpoint failure in memory; revocation reason persisted; no credentials included'
    $desktopAcceptance['real_client_known_issue']='dev.25 Z620 evidence confirms a policy-changed grant and a replacement grant; no-policy-change reauthorization remains unconfirmed; dev.26 awaits real-client renewal acceptance'
    $desktopAcceptance.observations+=@(@('acceptance.json','final-verification.json')|ForEach-Object{$path=Join-Path $desktopRoot "build/auth-refresh-validation/$_";@{name="dev25-$_";path=$path;sha256=(DesktopDigest $path)}})
    $desktopAcceptance.observations+=@(@('z620-auth-return-062759.json','z620-config2-20261021T063133Z.json')|ForEach-Object{$path=Join-Path $desktopRoot "build/real-client-acceptance-20261021/$_";@{name=$_;path=$path;sha256=(DesktopDigest $path)}})
    $desktopAcceptance.observations+=@(@('oauth-debug.json','oauth-release.json')|ForEach-Object{$path=Join-Path $desktopReports "preserved-before-scope-subset/$_";@{name="before-scope-subset-$_";path=$path;sha256=(DesktopDigest $path)}})
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath),($desktopAcceptance | ConvertTo-Json -Depth 12),$desktopUtf8)
Write-Output "Receipt: $OutputPath"
Write-Output "Status: $($desktopAcceptance.status); pending: $($desktopPending.Count)"
