param([string]$AcceptancePath = '')
$ErrorActionPreference='Stop'
$desktopRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
function DesktopPackageDigest([string]$Path) {(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
if (!$AcceptancePath) {$AcceptancePath=Join-Path $desktopRoot 'build/portable-validation/acceptance.json'}
$desktopAcceptance=Get-Content -LiteralPath $AcceptancePath -Raw -Encoding utf8 | ConvertFrom-Json
$desktopCommit=(& git -C $desktopRoot rev-parse HEAD).Trim()
if ($desktopAcceptance.version -ne '0.1.0-dev.23' -or $desktopAcceptance.stage -ne 'M16' -or $desktopAcceptance.status -ne 'portable_functional_verified' -or @($desktopAcceptance.pending).Count -or $desktopAcceptance.source_commit -ne $desktopCommit) {throw 'Current M16 desktop acceptance is required.'}
$desktopRequiredRelease=@('m1','m2','m3','m4','m5-ordinary','m6','m7','core','m9','m10','m10-backend','m11','m12','m13-files','m13-settings','http-response-boundary','desktop-app','desktop-entry','desktop-exit','oauth','oauth-ui','manager-refresh','portable')
foreach ($configuration in @('Debug','Release')) {
    $required=if($configuration -eq 'Release'){$desktopRequiredRelease}else{@('oauth','manager-refresh','portable')}
    foreach($stage in $required){
        $references=@($desktopAcceptance.reports | Where-Object {$_.stage -eq $stage -and $_.configuration -eq $configuration})
        if($references.Count -ne 1 -or $references[0].passed -isnot [bool] -or $references[0].passed -ne $true){throw "Missing or failed required desktop evidence: $configuration $stage"}
    }
}
foreach ($configuration in @('Debug','Release')) {
    foreach ($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll','ShellBot-TestFixture.exe','ShellBot-ProjectPolicyTests.exe')) {
        if ((DesktopPackageDigest (Join-Path $desktopRoot "out/x64/$configuration/$name")) -ne $desktopAcceptance.artifacts.$configuration.$name.sha256) {throw "Changed desktop product: $configuration $name"}
    }
}
if ((DesktopPackageDigest $desktopAcceptance.source_manifest.path) -ne $desktopAcceptance.source_manifest.sha256) {throw 'Desktop source manifest changed.'}
$desktopSource=Get-Content -LiteralPath $desktopAcceptance.source_manifest.path -Raw -Encoding utf8 | ConvertFrom-Json
foreach ($row in $desktopSource.files) {if ((DesktopPackageDigest (Join-Path $desktopRoot $row.relative_path)) -ne $row.sha256) {throw "Source changed after verification: $($row.relative_path)"}}
$desktopCurrentFiles=@(& git -C $desktopRoot -c core.quotepath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)
if ($LASTEXITCODE -or (Compare-Object $desktopCurrentFiles @($desktopSource.files.relative_path | Sort-Object -Unique))) {throw 'Desktop source set changed.'}
if ((DesktopPackageDigest $desktopAcceptance.local_test_manifest.path) -ne $desktopAcceptance.local_test_manifest.sha256) {throw 'Local test manifest changed.'}
$oauthTests=Get-Content -LiteralPath $desktopAcceptance.local_test_manifest.path -Raw -Encoding utf8 | ConvertFrom-Json
$oauthCurrentTests=@(Get-ChildItem -LiteralPath (Join-Path $desktopRoot 'tests') -File | Where-Object {$_.Extension -in @('.py','.c','.h','.vcxproj')})
if (Compare-Object @($oauthCurrentTests.Name | Sort-Object) @($oauthTests.files.name | Sort-Object)) {throw 'Local test set changed.'}
foreach ($row in $oauthTests.files) {if ((DesktopPackageDigest (Join-Path $desktopRoot "tests/$($row.name)")) -ne $row.sha256) {throw 'Local test changed after verification.'}}
foreach ($reference in $desktopAcceptance.reports) {
    if ((DesktopPackageDigest $reference.path) -ne $reference.sha256 -or !$reference.passed) {throw "Changed or failed desktop report: $($reference.stage)"}
    $value=Get-Content -LiteralPath $reference.path -Raw -Encoding utf8 | ConvertFrom-Json
    $hash=if ($value.sha256) {$value.sha256} elseif ($value.exe_sha256) {$value.exe_sha256} else {$value.artifact.sha256}
    if (!(Test-ValidationReportPassed $value) -or $hash -ne $desktopAcceptance.artifacts.($reference.configuration).'WinShellBot.exe'.sha256) {throw "Stale desktop evidence: $($reference.stage)"}
    if ($value.checks -and @($value.checks | Where-Object {$_.passed -isnot [bool] -or $_.passed -cne $true}).Count) {throw 'Failed nested validation check.'}
    if ($reference.stage -eq 'oauth') {
        if ($value.stage -ne 'M15-OAuth' -or $value.assertions -lt 250) {throw 'Complete current OAuth evidence is required.'}
        if ($reference.configuration -eq 'Release' -and $value.real_code_expiry_verified -cne $true) {throw 'Real authorization code expiry evidence is required.'}
        foreach ($name in @('OAuth metadata advertises public DCR and S256','real runtime reopens a valid encrypted OAuth store above 64 KiB','corrupt encrypted OAuth store fails closed with management UI available','real native revoke button and confirmation revoke active connection')) {
            if (@($value.checks | Where-Object {$_.name -ceq $name -and $_.passed -ceq $true}).Count -ne 1) {throw "Missing critical OAuth check: $name"}
        }
    }
    if ($reference.stage -eq 'portable' -and ($value.stage -ne 'M16-portable' -or $value.assertions -lt 60)) {throw 'Complete current portable migration evidence is required.'}
    if ($reference.stage -eq 'http-response-boundary' -and ($value.scope -ne 'responses-only-without-EOF' -or @($value.checks).Count -ne 216)) {throw 'Current HTTP response-only evidence is required.'}
    if ($reference.stage -eq 'portable') {
        foreach ($name in @('whole portable directory move rebases paths and retains content','failed copy does not publish a new storage location','C drive location refused before filesystem mutation','cancel exits without creating C drive or loading stale default data','fresh empty installation migrates before its first saved configuration','unavailable root can be explicitly replaced through the directory chooser','non-C alias resolving to C is refused before storage creation')) {
            if (@($value.checks | Where-Object {$_.name -ceq $name -and $_.passed -ceq $true}).Count -ne 1) {throw 'Missing critical portable migration check.'}
        }
    }
    if ($reference.stage -eq 'oauth-ui' -and ($value.stage -ne 'M15-OAuth-UI' -or $value.assertions -lt 19)) {throw 'Complete visible OAuth controls evidence is required.'}
    if ($reference.stage -eq 'manager-refresh' -and $value.assertions -lt 20) {throw 'Current manager reading position evidence is required.'}
    if($reference.stage -eq 'core' -and $value.scope -ne 'full'){throw 'Desktop package requires the full core suite.'}
    if($reference.stage -eq 'm13-files' -and $value.cross_directory_verified -ne $true){throw 'Desktop package requires native cross-directory file operations.'}
    if($reference.stage -eq 'desktop-app' -and ($value.stage -ne 'M14-desktop' -or $value.assertions -lt 64)){throw 'Complete current desktop workflow evidence is required.'}
    if($reference.stage -eq 'desktop-entry' -and ($value.stage -ne 'M14-entry' -or $value.assertions -lt 11 -or $value.entry_sha256 -ne $desktopAcceptance.artifacts.($reference.configuration).'ShellBot.exe'.sha256)){throw 'Current public desktop entry evidence is required.'}
    if($reference.stage -eq 'desktop-exit' -and ($value.stage -ne 'M14-exit' -or $value.assertions -lt 9)){throw 'Migration cancellation and complete exit evidence is required.'}
}
foreach($observation in $desktopAcceptance.observations) {
    if((DesktopPackageDigest $observation.path) -ne $observation.sha256){throw 'Known HTTP boundary observation changed.'}
}
# A test-only package must preserve failed network evidence without relabeling it.
$oauthNetwork=$desktopAcceptance.network_observation
if (!$oauthNetwork -or $oauthNetwork.complete -cne $true -or (DesktopPackageDigest $oauthNetwork.path) -ne $oauthNetwork.sha256) {throw 'Complete current network observation is required.'}
$tcp=Get-Content -LiteralPath $oauthNetwork.path -Raw -Encoding utf8 | ConvertFrom-Json
if ($tcp.exe_sha256 -ne $desktopAcceptance.artifacts.Release.'WinShellBot.exe'.sha256 -or $tcp.passed -isnot [bool] -or $tcp.passed -cne $oauthNetwork.passed -or $tcp.clean_exit -cne $true -or @($tcp.groups).Count -ne 5) {throw 'Invalid current network observation.'}
foreach ($group in @('fresh_keep_alive','close','split_close','rejected_close','close_concurrent')) {
    $rows=@($tcp.groups | Where-Object name -eq $group)
    if ($rows.Count -ne 1 -or $rows[0].calls -ne 200 -or $rows[0].failures -ne @($tcp.failures | Where-Object group -eq $group).Count) {throw 'Incomplete current network observation.'}
}
if (($tcp.passed -cne (@($tcp.failures).Count -eq 0)) -or $oauthNetwork.failures -ne @($tcp.failures).Count) {throw 'Contradictory network observation.'}
$boundary=$desktopAcceptance.network_boundary
if (!$boundary -or (DesktopPackageDigest $boundary.path) -ne $boundary.sha256) {throw 'Current EOF boundary observation is required.'}
$boundaryValue=Get-Content -LiteralPath $boundary.path -Raw -Encoding utf8 | ConvertFrom-Json
if ($boundaryValue.sha256 -ne $desktopAcceptance.artifacts.Release.'WinShellBot.exe'.sha256 -or $boundaryValue.passed -isnot [bool] -or $boundaryValue.passed -cne $boundary.passed) {throw 'EOF boundary observation was misrepresented.'}
$desktopPdfium=Get-Content -LiteralPath (Join-Path $desktopRoot 'third_party/pdfium/provenance.json') -Raw -Encoding utf8 | ConvertFrom-Json
$desktopPdfiumAsset=@($desktopPdfium.assets | Where-Object architecture -eq 'x64')
if ($desktopPdfiumAsset.Count -ne 1 -or (DesktopPackageDigest (Join-Path $desktopRoot 'out/x64/Release/pdfium.dll')) -ne $desktopPdfiumAsset[0].dll_sha256) {throw 'PDFium differs from pinned x64 dependency.'}
$desktopVersion=& (Join-Path $desktopRoot 'out/x64/Release/WinShellBot.exe') --version
if ($LASTEXITCODE -or $desktopVersion -ne 'ShellBot 0.1.0-dev.23 (C17)') {throw 'Unexpected desktop version.'}
$desktopName='ShellBot-0.1.0-dev.23-windows-x64-portable-test-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$desktopStage=Join-Path $desktopRoot "dist/$desktopName";$desktopArchive="$desktopStage.zip"
if ((Test-Path -LiteralPath $desktopStage) -or (Test-Path -LiteralPath $desktopArchive)) {throw 'Desktop package destination exists.'}
[void][IO.Directory]::CreateDirectory($desktopStage)
$desktopFiles=[ordered]@{}
foreach ($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')) {$desktopFiles[$name]=Join-Path $desktopRoot "out/x64/Release/$name"}
$desktopFiles['LICENSE.txt']=Join-Path $desktopRoot 'LICENSE.txt'
foreach ($name in @('FILE_MANAGEMENT.md','FILE_EDIT.md','EXECUTION_PERMISSIONS.md','COMMAND_TASKS.md','REMOTE_APPROVALS.md','IMAGE_VIEW.md','DOCUMENT_READING.md','ARTIFACT_TRANSFER.md')) {$desktopFiles["docs/$name"]=Join-Path $desktopRoot "docs/$name"}
$desktopFiles['licenses/yyjson-MIT.txt']=Join-Path $desktopRoot 'third_party/yyjson/LICENSE'
$desktopFiles['licenses/CivetWeb-MIT.txt']=Join-Path $desktopRoot 'third_party/civetweb/LICENSE.md'
$desktopFiles['licenses/PDFium-binaries-MIT.txt']=Join-Path $desktopRoot 'third_party/pdfium/LICENSE'
$desktopFiles['licenses/PDFium-provenance.json']=Join-Path $desktopRoot 'third_party/pdfium/provenance.json'
foreach ($license in Get-ChildItem -LiteralPath (Join-Path $desktopRoot 'third_party/pdfium/licenses') -File) {$desktopFiles["licenses/pdfium/$($license.Name)"]=$license.FullName}
$desktopFiles['validation/acceptance.json']=$AcceptancePath
$desktopFiles['validation/source-manifest.json']=$desktopAcceptance.source_manifest.path
$desktopFiles['validation/network-observation.json']=$oauthNetwork.path
$desktopFiles['validation/current-http-eof.json']=$boundary.path
$desktopFiles['validation/local-test-manifest.json']=$desktopAcceptance.local_test_manifest.path
foreach ($ref in $desktopAcceptance.reports) {$desktopFiles["validation/reports/$(Split-Path -Leaf $ref.path)"]=$ref.path}
foreach($observation in $desktopAcceptance.observations){$desktopFiles["validation/observations/$($observation.name)"]=$observation.path}
foreach ($entry in $desktopFiles.GetEnumerator()) {
    $target=Join-Path $desktopStage $entry.Key;[void][IO.Directory]::CreateDirectory((Split-Path -Parent $target));Copy-Item -LiteralPath $entry.Value -Destination $target
}
$desktopUtf8=[Text.UTF8Encoding]::new($false)
$desktopGuide=Get-Content -LiteralPath (Join-Path $desktopRoot 'docs/USER_GUIDE.md') -Raw -Encoding utf8
$desktopHistory=$desktopGuide.IndexOf('## 历史：',[StringComparison]::Ordinal)
if($desktopHistory -ge 0){$desktopGuide=$desktopGuide.Substring(0,$desktopHistory)}
$desktopGuide=$desktopGuide.Trim()+[Environment]::NewLine
[IO.File]::WriteAllText((Join-Path $desktopStage 'docs/USER_GUIDE.md'),$desktopGuide,$desktopUtf8)
[IO.File]::WriteAllText((Join-Path $desktopStage 'README.md'),@"
# ShellBot 0.1.0-dev.23 便携版

完整解压到可写的非 C 盘目录，双击 **ShellBot.exe**。默认配置、项目、凭据、日志、审计、产物、缓存和临时目录均位于程序旁的 **Data**。不会读取或新建旧 LocalAppData 配置。

在管理界面新建项目。连接设置填写已有本机端口和 HTTPS 公网域名，OAuth 自动启用。复制项目公网 MCP 地址，在客户端选择 OAuth 和自动动态注册；浏览器申请后，在本机“客户端授权…”核对并允许，无需填写 token 或客户端密钥。

“数据位置…”可修改应用数据目录。程序先复制并逐文件校验，再保存新位置。点击“完成并退出”后重新打开，确认新目录正常后手动删除旧目录。使用默认 Data 时，可以退出后移动整个程序文件夹；程序会更新内部路径。凭据仍绑定原 Windows 账户。

这是 M16 便携与 OAuth 客户端测试包。准确功能回归和程序摘要见 validation/acceptance.json。当前 TCP EOF 边界检查仍失败，历史连接异常也未确认根因，真实 MCP 客户端、新版本实际管理员与独立机器仍待验收。使用说明见 docs/USER_GUIDE.md。

相邻 WinShellBot.exe、ShellBot-SandboxProbe.exe 和 pdfium.dll 为运行组件。无需日常控制脚本；关闭窗口收进托盘，托盘右键退出停止全部项目。
"@,$desktopUtf8)
[IO.File]::WriteAllText((Join-Path $desktopStage '开始使用.txt'),@"
1. 从托盘退出旧管理器，解压到可写的非 C 盘目录，运行 ShellBot.exe。
2. 默认数据在程序旁的 Data；界面的“数据位置…”支持复制校验迁移。
3. 新建项目，设置本机监听端口和 HTTPS 公网域名。
4. 客户端选择 OAuth 和自动动态注册，粘贴项目公网 MCP 地址。
5. 浏览器申请后，在本机“客户端授权…”核对并允许。
6. 关闭窗口收进托盘；右键退出停止全部项目。完整说明见 docs/USER_GUIDE.md。
"@,$desktopUtf8)
[IO.File]::WriteAllText((Join-Path $desktopStage '验证说明.txt'),@"
便携默认目录、非 C 盘存储、真实 GUI 复制迁移、逐文件校验、失败回退、重启、整目录移动、空项目迁移、不可用位置询问及取消流程均以随包准确程序摘要绑定的报告为准。普通和受限命令也验证非 C 盘缓存环境与真实临时文件创建，原有 LPAC 与项目权限边界保留。
OAuth、管理界面、阅读位置和完整普通回归见 validation/reports。当前 HTTP 响应压测原始结果见 validation/network-observation.json，当前 EOF 失败见 validation/current-http-eof.json，历史失败和独立对照见 validation/observations；未将历史连接及 EOF 问题标为已解决。
真实 MCP 客户端、另一电脑、新版本实际 UAC、物理托盘和长期网络稳定性继续分别待验。本包为便携客户端测试包。
"@,$desktopUtf8)
$desktopManifest=[ordered]@{
    stage='M16';version='0.1.0-dev.23';package_kind='portable-test';default_entry='ShellBot.exe';control_cmd_scripts=$false
    source_commit=$desktopCommit;source_local_changes=$desktopAcceptance.working_tree_dirty;pushed=$false
    source_manifest_sha256=$desktopAcceptance.source_manifest.sha256;acceptance_sha256=(DesktopPackageDigest $AcceptancePath)
    executable_sha256=$desktopAcceptance.artifacts.Release.'WinShellBot.exe'.sha256;entry_sha256=$desktopAcceptance.artifacts.Release.'ShellBot.exe'.sha256
    tools=37;compiler_language='C17';runtime='static MSVC CRT';bundled_data=$false;bundled_credentials=$false
    portable=$desktopAcceptance.portable
    oauth=$desktopAcceptance.oauth
    lifecycle=$desktopAcceptance.lifecycle;real_mcp_client='pending';external_systems='pending';fresh_administrator_uac='pending';physical_mouse_tray_acceptance='pending'
    http_response_stress=$desktopAcceptance.http_response_stress;all_regression_passed=$false
    http_eof_stability=$desktopAcceptance.http_eof_stability;known_limitations=$desktopAcceptance.known_limitations
    license_status='Full third-party notices included; project copyright placeholder remains before formal release'
}
[IO.File]::WriteAllText((Join-Path $desktopStage 'manifest.json'),($desktopManifest | ConvertTo-Json -Depth 8),$desktopUtf8)
$desktopHashes=@(Get-ChildItem -LiteralPath $desktopStage -Recurse -File | Sort-Object FullName | ForEach-Object {@{path=$_.FullName.Substring($desktopStage.Length+1).Replace('\','/');sha256=(DesktopPackageDigest $_.FullName)}})
if ($desktopHashes.path -match '(^|/)(Data|tests|config)(/|$)|\.(cmd|bat|ps1|py|pdb|sbproj|sbkey|dpapi)$|TestFixture|SettingsUiHost') {throw 'Desktop package contains scripts, runtime data, credentials or development files.'}
if (@(Get-ChildItem -LiteralPath (Join-Path $desktopStage 'licenses/pdfium') -File).Count -ne 17) {throw 'PDFium upstream notices are incomplete.'}
[IO.File]::WriteAllText((Join-Path $desktopStage 'SHA256SUMS.json'),(ConvertTo-Json -InputObject $desktopHashes -Depth 5),$desktopUtf8)
Compress-Archive -LiteralPath $desktopStage -DestinationPath $desktopArchive -CompressionLevel Optimal
$desktopArchiveHash=DesktopPackageDigest $desktopArchive
[IO.File]::WriteAllText("$desktopArchive.sha256","$desktopArchiveHash  $desktopName.zip`n",$desktopUtf8)
$desktopReceipt=@{stage='M16';status='packaged_pending_extracted_runtime';kind='portable-test';archive=$desktopArchive;archive_sha256=$desktopArchiveHash;directory=$desktopStage;source_commit=$desktopCommit;source_manifest_sha256=$desktopAcceptance.source_manifest.sha256;executable_sha256=$desktopManifest.executable_sha256;entry_sha256=$desktopManifest.entry_sha256;files=$desktopHashes.Count;acceptance=@{path=$AcceptancePath;sha256=(DesktopPackageDigest $AcceptancePath)}}
[IO.File]::WriteAllText((Join-Path $desktopRoot 'build/portable-validation/package-receipt.json'),($desktopReceipt | ConvertTo-Json -Depth 6),$desktopUtf8)
Write-Output "Package: $desktopArchive";Write-Output "SHA256: $desktopArchiveHash"
