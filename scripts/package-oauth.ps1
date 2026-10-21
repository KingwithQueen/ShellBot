param([string]$AcceptancePath = '')
$ErrorActionPreference='Stop'
$desktopRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
function DesktopPackageDigest([string]$Path) {(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
if (!$AcceptancePath) {$AcceptancePath=Join-Path $desktopRoot 'build/oauth-validation/acceptance.json'}
$desktopAcceptance=Get-Content -LiteralPath $AcceptancePath -Raw -Encoding utf8 | ConvertFrom-Json
$desktopCommit=(& git -C $desktopRoot rev-parse HEAD).Trim()
if ($desktopAcceptance.version -ne '0.1.0-dev.22' -or $desktopAcceptance.stage -ne 'M15' -or $desktopAcceptance.status -ne 'oauth_functional_verified' -or @($desktopAcceptance.pending).Count -or $desktopAcceptance.source_commit -ne $desktopCommit) {throw 'Current M15 desktop acceptance is required.'}
$desktopRequiredRelease=@('m1','m2','m3','m4','m5-ordinary','m6','m7','core','m9','m10','m10-backend','m11','m12','m13-files','m13-settings','http-boundary','desktop-app','desktop-entry','desktop-exit','oauth','oauth-ui','manager-refresh')
foreach ($configuration in @('Debug','Release')) {
    $required=if($configuration -eq 'Release'){$desktopRequiredRelease}else{@('oauth','manager-refresh')}
    foreach($stage in $required){
        $references=@($desktopAcceptance.reports | Where-Object {$_.stage -eq $stage -and $_.configuration -eq $configuration})
        if($references.Count -ne 1 -or $references[0].passed -isnot [bool] -or $references[0].passed -ne $true){throw "Missing or failed required desktop evidence: $configuration $stage"}
    }
}
foreach ($configuration in @('Debug','Release')) {
    foreach ($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')) {
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
    if ($rows.Count -ne 1 -or $rows[0].calls -ne 1000 -or $rows[0].failures -ne @($tcp.failures | Where-Object group -eq $group).Count) {throw 'Incomplete current network observation.'}
}
if (($tcp.passed -cne (@($tcp.failures).Count -eq 0)) -or $oauthNetwork.failures -ne @($tcp.failures).Count) {throw 'Contradictory network observation.'}
$desktopPdfium=Get-Content -LiteralPath (Join-Path $desktopRoot 'third_party/pdfium/provenance.json') -Raw -Encoding utf8 | ConvertFrom-Json
$desktopPdfiumAsset=@($desktopPdfium.assets | Where-Object architecture -eq 'x64')
if ($desktopPdfiumAsset.Count -ne 1 -or (DesktopPackageDigest (Join-Path $desktopRoot 'out/x64/Release/pdfium.dll')) -ne $desktopPdfiumAsset[0].dll_sha256) {throw 'PDFium differs from pinned x64 dependency.'}
$desktopVersion=& (Join-Path $desktopRoot 'out/x64/Release/WinShellBot.exe') --version
if ($LASTEXITCODE -or $desktopVersion -ne 'ShellBot 0.1.0-dev.22 (C17)') {throw 'Unexpected desktop version.'}
$desktopName='ShellBot-0.1.0-dev.22-windows-x64-oauth-test-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
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
# ShellBot 0.1.0-dev.22

完整解压后双击 **ShellBot.exe**。在管理界面新建项目并选择存储目录；关闭窗口收进托盘，右键退出停止全部项目。

已有项目与 HTTPS 隧道：在连接设置填写 HTTPS 公网地址（只填写域名，不带 /projects 路径），OAuth 自动启用。复制项目的公网 MCP 地址，在客户端选择 OAuth 和自动动态注册。浏览器申请后，在管理界面的“客户端授权…”核对项目、客户端与回调地址，允许连接。无需填写项目令牌、客户端 ID 或密钥。续期自动完成，可以随时撤销。

这是 M15 OAuth 测试包。完整普通回归、管理界面及本机 OAuth HTTP 授权已验证，真实 MCP 客户端和另一电脑仍待验收。当前 5000 次 HTTP 压测有 3 次连接超时，历史 TCP EOF 边界另有未解决观察；这是带网络待验项的测试包，未标记为全回归通过的候选版。详见 manifest.json、validation/acceptance.json 和 [使用说明](docs/USER_GUIDE.md)。

相邻 WinShellBot.exe、ShellBot-SandboxProbe.exe 和 pdfium.dll 是运行组件，请保留。
"@,$desktopUtf8)
[IO.File]::WriteAllText((Join-Path $desktopStage '开始使用.txt'),@"
1. 从旧管理器托盘菜单退出，完整解压本包，双击 ShellBot.exe。
2. 已有项目由原 Windows 用户读取；新项目在管理界面中新建并选择独立存储目录。
3. 连接设置保留已有本机端口和 HTTPS 公网地址，OAuth 自动启用。
4. 复制项目公网 MCP 地址，在客户端选择 OAuth、自动动态注册，不必填写令牌或密钥。
5. 浏览器提出连接申请后，在本机“客户端授权…”核对并允许；返回浏览器继续。
6. 右键托盘退出会停止全部项目。详细说明见 docs/USER_GUIDE.md。
"@,$desktopUtf8)
[IO.File]::WriteAllText((Join-Path $desktopStage '验证说明.txt'),@"
本机 OAuth 授权、S256 PKCE、项目隔离、真实 MCP 文件写入、自动续期、旧刷新凭据重放撤销、过期、重启持久化、损坏加密存储拒绝、原生授权窗口、阅读位置保持及完整普通回归通过。各份报告绑定准确的程序 SHA-256，随包保存在 validation/reports。
真实 MCP 客户端、另一电脑、新版本实际管理员专项与物理托盘操作仍单独待验。
当前 5000 次 HTTP 响应压力测试有 3 次连接超时。旧版程序与独立 Python 服务也有类型不同的异常，不能据此排除程序问题或确定系统根因。当前失败、对照和历史 TCP EOF 观察均保留于 validation/observations 和 validation/network-observation.json；网络稳定性仍未通过。
"@,$desktopUtf8)
$desktopManifest=[ordered]@{
    stage='M15';version='0.1.0-dev.22';package_kind='oauth-test';default_entry='ShellBot.exe';control_cmd_scripts=$false
    source_commit=$desktopCommit;source_local_changes=$desktopAcceptance.working_tree_dirty;pushed=$false
    source_manifest_sha256=$desktopAcceptance.source_manifest.sha256;acceptance_sha256=(DesktopPackageDigest $AcceptancePath)
    executable_sha256=$desktopAcceptance.artifacts.Release.'WinShellBot.exe'.sha256;entry_sha256=$desktopAcceptance.artifacts.Release.'ShellBot.exe'.sha256
    tools=37;compiler_language='C17';runtime='static MSVC CRT';bundled_data=$false;bundled_credentials=$false
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
$desktopReceipt=@{stage='M15';status='packaged_pending_extracted_runtime';kind='oauth-test';archive=$desktopArchive;archive_sha256=$desktopArchiveHash;directory=$desktopStage;source_commit=$desktopCommit;source_manifest_sha256=$desktopAcceptance.source_manifest.sha256;executable_sha256=$desktopManifest.executable_sha256;entry_sha256=$desktopManifest.entry_sha256;files=$desktopHashes.Count;acceptance=@{path=$AcceptancePath;sha256=(DesktopPackageDigest $AcceptancePath)}}
[IO.File]::WriteAllText((Join-Path $desktopRoot 'build/oauth-validation/package-receipt.json'),($desktopReceipt | ConvertTo-Json -Depth 6),$desktopUtf8)
Write-Output "Package: $desktopArchive";Write-Output "SHA256: $desktopArchiveHash"
