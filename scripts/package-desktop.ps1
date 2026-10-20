param([string]$AcceptancePath = '')
$ErrorActionPreference='Stop'
$desktopRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'validation-report.ps1')
function DesktopPackageDigest([string]$Path) {(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
if (!$AcceptancePath) {$AcceptancePath=Join-Path $desktopRoot 'build/desktop-validation/acceptance.json'}
$desktopAcceptance=Get-Content -LiteralPath $AcceptancePath -Raw -Encoding utf8 | ConvertFrom-Json
$desktopCommit=(& git -C $desktopRoot rev-parse HEAD).Trim()
if ($desktopAcceptance.stage -ne 'M14' -or $desktopAcceptance.status -ne 'desktop_local_verified' -or @($desktopAcceptance.pending).Count -or $desktopAcceptance.source_commit -ne $desktopCommit) {throw 'Current M14 desktop acceptance is required.'}
$desktopRequiredRelease=@('m1','m2','m3','m4','m5-ordinary','m6','m7','core','m9','m10','m10-backend','m11','m12','m13-files','m13-settings','http-boundary','http-close','desktop-app','desktop-entry','desktop-exit')
foreach ($configuration in @('Debug','Release')) {
    $required=if($configuration -eq 'Release'){$desktopRequiredRelease}else{@('desktop-app','desktop-entry','desktop-exit')}
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
foreach ($reference in $desktopAcceptance.reports) {
    if ((DesktopPackageDigest $reference.path) -ne $reference.sha256 -or !$reference.passed) {throw "Changed or failed desktop report: $($reference.stage)"}
    $value=Get-Content -LiteralPath $reference.path -Raw -Encoding utf8 | ConvertFrom-Json
    $hash=if ($value.sha256) {$value.sha256} elseif ($value.exe_sha256) {$value.exe_sha256} else {$value.artifact.sha256}
    if (!(Test-ValidationReportPassed $value) -or $hash -ne $desktopAcceptance.artifacts.($reference.configuration).'WinShellBot.exe'.sha256) {throw "Stale desktop evidence: $($reference.stage)"}
    if($reference.stage -eq 'core' -and $value.scope -ne 'full'){throw 'Desktop package requires the full core suite.'}
    if($reference.stage -eq 'm13-files' -and $value.cross_directory_verified -ne $true){throw 'Desktop package requires native cross-directory file operations.'}
    if($reference.stage -eq 'desktop-app' -and ($value.stage -ne 'M14-desktop' -or $value.assertions -lt 64)){throw 'Complete current desktop workflow evidence is required.'}
    if($reference.stage -eq 'desktop-entry' -and ($value.stage -ne 'M14-entry' -or $value.assertions -lt 11 -or $value.entry_sha256 -ne $desktopAcceptance.artifacts.($reference.configuration).'ShellBot.exe'.sha256)){throw 'Current public desktop entry evidence is required.'}
    if($reference.stage -eq 'desktop-exit' -and ($value.stage -ne 'M14-exit' -or $value.assertions -lt 9)){throw 'Migration cancellation and complete exit evidence is required.'}
    if($reference.stage -eq 'http-close') {
        $groups=@('fresh_keep_alive','close','split_close','rejected_close','close_concurrent')
        if(@($value.groups).Count -ne $groups.Count -or $value.clean_exit -ne $true -or @($value.failures).Count){throw 'Complete clean HTTP stress evidence is required.'}
        foreach($group in $groups) {
            $rows=@($value.groups | Where-Object name -eq $group)
            if($rows.Count -ne 1 -or $rows[0].calls -ne 1000 -or $rows[0].failures -ne 0){throw "Incomplete HTTP stress group: $group"}
        }
    }
}
foreach($observation in $desktopAcceptance.observations) {
    if((DesktopPackageDigest $observation.path) -ne $observation.sha256){throw 'Known HTTP boundary observation changed.'}
}
$desktopPdfium=Get-Content -LiteralPath (Join-Path $desktopRoot 'third_party/pdfium/provenance.json') -Raw -Encoding utf8 | ConvertFrom-Json
$desktopPdfiumAsset=@($desktopPdfium.assets | Where-Object architecture -eq 'x64')
if ($desktopPdfiumAsset.Count -ne 1 -or (DesktopPackageDigest (Join-Path $desktopRoot 'out/x64/Release/pdfium.dll')) -ne $desktopPdfiumAsset[0].dll_sha256) {throw 'PDFium differs from pinned x64 dependency.'}
$desktopVersion=& (Join-Path $desktopRoot 'out/x64/Release/WinShellBot.exe') --version
if ($LASTEXITCODE -or $desktopVersion -ne 'ShellBot 0.1.0-dev.21 (C17)') {throw 'Unexpected desktop version.'}
$desktopName='ShellBot-0.1.0-dev.21-windows-x64-desktop-test-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
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
[IO.File]::WriteAllText((Join-Path $desktopStage 'README.md'),"# ShellBot 0.1.0-dev.21`r`n`r`n完整解压后双击 **ShellBot.exe**，在管理界面中新建项目、选择存储目录并配置本机端口。程序自动生成并加密保存项目凭据。`r`n`r`n关闭窗口收进托盘，右键选择退出后停止全部项目。WinShellBot.exe、ShellBot-SandboxProbe.exe 和 pdfium.dll 是相邻运行组件，请保留。`r`n`r`n操作步骤见 [使用说明](docs/USER_GUIDE.md)。此包为 M14 桌面测试包；本机已验证项目创建、配置、目录迁移、托盘和完整退出。真实 MCP 客户端、另一电脑以及新版本实际管理员专项继续单独验收，状态见 manifest.json 和 validation/acceptance.json。`r`n",$desktopUtf8)
[IO.File]::WriteAllText((Join-Path $desktopStage '开始使用.txt'),"完整解压后双击 ShellBot.exe。`r`n在管理界面中新建项目，选择独立存储目录，即可保存并运行。`r`n连接设置中填写已有隧道的本机端口，选中项目复制地址和项目令牌。`r`n关闭窗口收进托盘；右键退出后全部项目与任务停止。`r`n详细步骤请阅读 docs/USER_GUIDE.md。`r`n此包为 dev.21 桌面测试包，真实 MCP 客户端与新版本实际管理员专项继续单独验收。`r`n",$desktopUtf8)
[IO.File]::WriteAllText((Join-Path $desktopStage '验证说明.txt'),"桌面新建、配置、目录迁移、托盘和退出流程已经本机验证。`r`n完整普通回归、5000 次 HTTP 响应调用已通过，但重复等待 TCP EOF 的边界检查另有失败；独立 Python 服务也复现异常，根因未确认。相关失败记录保留于 validation/observations。此包保留原 HTTP 实现，没有将实验性网络改动打入产品。`r`n请继续在待测电脑和真实 MCP 客户端验收，不将本机桌面验证当作网络稳定性或新版本实际管理员验收。`r`n",$desktopUtf8)
$desktopManifest=[ordered]@{
    stage='M14';version='0.1.0-dev.21';package_kind='desktop-test';default_entry='ShellBot.exe';control_cmd_scripts=$false
    source_commit=$desktopCommit;source_local_changes=$desktopAcceptance.working_tree_dirty;pushed=$false
    source_manifest_sha256=$desktopAcceptance.source_manifest.sha256;acceptance_sha256=(DesktopPackageDigest $AcceptancePath)
    executable_sha256=$desktopAcceptance.artifacts.Release.'WinShellBot.exe'.sha256;entry_sha256=$desktopAcceptance.artifacts.Release.'ShellBot.exe'.sha256
    tools=37;compiler_language='C17';runtime='static MSVC CRT';bundled_data=$false;bundled_credentials=$false
    lifecycle=$desktopAcceptance.lifecycle;real_mcp_client='pending';external_systems='pending';fresh_administrator_uac='pending';physical_mouse_tray_acceptance='pending'
    http_eof_stability=$desktopAcceptance.http_eof_stability;known_limitations=$desktopAcceptance.known_limitations
    license_status='Full third-party notices included; project copyright placeholder remains before formal release'
}
[IO.File]::WriteAllText((Join-Path $desktopStage 'manifest.json'),($desktopManifest | ConvertTo-Json -Depth 8),$desktopUtf8)
$desktopHashes=@(Get-ChildItem -LiteralPath $desktopStage -Recurse -File | Sort-Object FullName | ForEach-Object {@{path=$_.FullName.Substring($desktopStage.Length+1).Replace('\','/');sha256=(DesktopPackageDigest $_.FullName)}})
if ($desktopHashes.path -match '(^|/)(Data|tests|config)(/|$)|\.(cmd|bat|ps1|py|pdb|sbproj|sbkey)$|TestFixture|SettingsUiHost') {throw 'Desktop package contains scripts, runtime data, credentials or development files.'}
if (@(Get-ChildItem -LiteralPath (Join-Path $desktopStage 'licenses/pdfium') -File).Count -ne 17) {throw 'PDFium upstream notices are incomplete.'}
[IO.File]::WriteAllText((Join-Path $desktopStage 'SHA256SUMS.json'),(ConvertTo-Json -InputObject $desktopHashes -Depth 5),$desktopUtf8)
Compress-Archive -LiteralPath $desktopStage -DestinationPath $desktopArchive -CompressionLevel Optimal
$desktopArchiveHash=DesktopPackageDigest $desktopArchive
[IO.File]::WriteAllText("$desktopArchive.sha256","$desktopArchiveHash  $desktopName.zip`n",$desktopUtf8)
$desktopReceipt=@{stage='M14';status='packaged_pending_extracted_runtime';kind='desktop-test';archive=$desktopArchive;archive_sha256=$desktopArchiveHash;directory=$desktopStage;source_commit=$desktopCommit;source_manifest_sha256=$desktopAcceptance.source_manifest.sha256;executable_sha256=$desktopManifest.executable_sha256;entry_sha256=$desktopManifest.entry_sha256;files=$desktopHashes.Count;acceptance=@{path=$AcceptancePath;sha256=(DesktopPackageDigest $AcceptancePath)}}
[IO.File]::WriteAllText((Join-Path $desktopRoot 'build/desktop-validation/package-receipt.json'),($desktopReceipt | ConvertTo-Json -Depth 6),$desktopUtf8)
Write-Output "Package: $desktopArchive";Write-Output "SHA256: $desktopArchiveHash"
