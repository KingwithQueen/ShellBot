param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$validation=Join-Path $root 'build\native-ui-validation'
$env:TEMP=Join-Path $validation 'Temp';$env:TMP=$env:TEMP
$utf8=[Text.UTF8Encoding]::new($false)
function Digest([string]$path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
function WriteJson([string]$path,$value){[IO.File]::WriteAllText($path,($value|ConvertTo-Json -Depth 12),$utf8)}
$spec=[ordered]@{'native-ui'='M20-native-ui';'native-dialogs'='M20-native-dialogs';'refresh'='M14-manager-refresh';'oauth-ui'='M15-OAuth-UI';'oauth-refresh-ui'='M20-oauth-refresh-ui';'oauth'='M15-OAuth';'desktop'='M14-desktop';'entry'='M14-entry';'exit'='M14-exit';'settings'='M13-settings';'management'='M7';'portable'='M16-portable';'core'=''}
$reports=@();$artifacts=[ordered]@{}
foreach($configuration in @('Debug','Release')){
    $binaries=[ordered]@{}
    foreach($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')){
        $path=Join-Path $root "out\x64\$configuration\$name";$binaries[$name]=@{sha256=(Digest $path);bytes=(Get-Item -LiteralPath $path).Length}
    }
    $artifacts[$configuration]=$binaries
    $names=if($configuration -eq 'Debug'){@('native-ui','native-dialogs','refresh','oauth-ui','oauth-refresh-ui','oauth')}else{@($spec.Keys)}
    foreach($name in $names){
        $path=Join-Path $validation ($configuration.ToLower()+'-'+$name+'.json');$value=Get-Content -LiteralPath $path -Raw -Encoding utf8|ConvertFrom-Json
        $hash=if($value.sha256){$value.sha256}elseif($value.artifact.sha256){$value.artifact.sha256}else{$value.exe_sha256}
        if(($value.passed -cne $true -and $value.status -cne 'passed') -or $hash -ne $binaries['WinShellBot.exe'].sha256){throw "Failed or stale report: $path"}
        if($spec[$name] -and $value.stage -cne $spec[$name]){throw "Wrong test stage: $path"}
        if($null -ne $value.checks -and @($value.checks|Where-Object {$_.passed -cne $true}).Count){throw "Failed nested check: $path"}
        if($name -eq 'core' -and ($value.scope -cne 'full' -or $value.close_probe.failures -ne 0)){throw 'Full current core regression is required.'}
        if($name -eq 'oauth' -and ($value.long_term_authorization_verified -cne $true -or $value.storage_format -ne 3 -or ($configuration -eq 'Release' -and $value.real_code_expiry_verified -cne $true))){throw 'Current persistent OAuth and real code-expiry evidence is required.'}
        if($name -eq 'entry' -and $value.entry_sha256 -ne $binaries['ShellBot.exe'].sha256){throw 'Launcher report is stale.'}
        $count=if($value.assertions){$value.assertions}elseif($value.checks_count){$value.checks_count}elseif($value.assertions_passed){$value.assertions_passed}else{@($value.checks).Count}
        $reports+=@{configuration=$configuration;name=$name;passed=$true;checks=$count;path=$path;sha256=(Digest $path);executable_sha256=$hash}
    }
}
$version=& (Join-Path $root 'out\x64\Release\WinShellBot.exe') --version
if($LASTEXITCODE -or $version -cne 'ShellBot 0.1.0-dev.27 (C17)'){throw 'Unexpected UI candidate version.'}
$commit=(& git -C $root rev-parse HEAD).Trim()
$paths=@(& git -C $root -c core.quotepath=false ls-files --cached --others --exclude-standard|Sort-Object -Unique)
if($LASTEXITCODE){throw 'Cannot inventory source.'}
$source=@($paths|ForEach-Object {@{path=$_;sha256=(Digest (Join-Path $root $_))}})
$tests=@(Get-ChildItem -LiteralPath (Join-Path $root 'tests') -File|Where-Object {$_.Extension -in @('.py','.c','.h','.vcxproj')}|Sort-Object Name|ForEach-Object {@{name=$_.Name;sha256=(Digest $_.FullName)}})
WriteJson (Join-Path $validation 'source-manifest.json') @{source_commit=$commit;files=$source}
WriteJson (Join-Path $validation 'local-test-manifest.json') @{files=$tests}
$acceptance=@{stage='M20';status='native_ui_local_verified';version='0.1.0-dev.27';source_commit=$commit;working_tree_dirty=$true;pushed=$false;artifacts=$artifacts;reports=$reports;source_manifest_sha256=(Digest (Join-Path $validation 'source-manifest.json'));test_manifest_sha256=(Digest (Join-Path $validation 'local-test-manifest.json'));pending=@('real_client','fresh_administrator_uac','physical_multi_monitor_dpi','independent_windows_systems','long_running_acceptance');http_stress='M19 frozen evidence retained; this UI candidate repeats core HTTP and 100 close probes, not the full M19 EOF stress'}
WriteJson (Join-Path $validation 'acceptance.json') $acceptance
$name='ShellBot-0.1.0-dev.27-windows-x64-native-ui-test-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$stage=Join-Path $root ('dist\'+$name);$archive=$stage+'.zip'
if((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)){throw 'Package destination already exists.'}
$files=[ordered]@{}
foreach($file in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')){$files[$file]=Join-Path $root ('out\x64\Release\'+$file)}
$files['LICENSE.txt']=Join-Path $root 'LICENSE.txt'
foreach($file in @('USER_GUIDE.md','UI_REVIEW_20261021.md','HANDOFF_20261021_NATIVE_UI.md','FILE_MANAGEMENT.md','FILE_EDIT.md','EXECUTION_PERMISSIONS.md','COMMAND_TASKS.md','REMOTE_APPROVALS.md','IMAGE_VIEW.md','DOCUMENT_READING.md','ARTIFACT_TRANSFER.md')){$files['docs/'+$file]=Join-Path $root ('docs\'+$file)}
$files['licenses/yyjson-MIT.txt']=Join-Path $root 'third_party\yyjson\LICENSE'
$files['licenses/CivetWeb-MIT.txt']=Join-Path $root 'third_party\civetweb\LICENSE.md'
$files['licenses/PDFium-binaries-MIT.txt']=Join-Path $root 'third_party\pdfium\LICENSE'
$files['licenses/PDFium-provenance.json']=Join-Path $root 'third_party\pdfium\provenance.json'
foreach($file in Get-ChildItem -LiteralPath (Join-Path $root 'third_party\pdfium\licenses') -File){$files['licenses/pdfium/'+$file.Name]=$file.FullName}
foreach($file in @('acceptance.json','source-manifest.json','local-test-manifest.json')){$files['validation/'+$file]=Join-Path $validation $file}
foreach($entry in $files.GetEnumerator()){$target=Join-Path $stage $entry.Key;[IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null;Copy-Item -LiteralPath $entry.Value -Destination $target}
[IO.File]::WriteAllText((Join-Path $stage 'README.md'),"# ShellBot 0.1.0-dev.27 原生管理界面候选`n`n完整解压到非 C 盘目录，双击 ShellBot.exe。系统菜单与分组操作栏用于项目和连接管理。关闭窗口收进托盘；从菜单或托盘退出停止全部项目。操作详见 docs/USER_GUIDE.md。`n`n本机界面与相关真实后端回归已通过。当前是后续正式版界面的开发候选；真实客户端、物理跨屏 DPI、实际 UAC 和长期运行仍待验。准确范围见 validation/acceptance.json 和 docs/UI_REVIEW_20261021.md。旧 dev.26 包和原始记录保留。`n",$utf8)
[IO.File]::WriteAllText((Join-Path $stage '开始使用.txt'),"完整解压到非 C 盘目录后双击 ShellBot.exe。`r`n先从托盘退出旧版，备份原 Data 和 ShellBot.storage.json。可替换原固定程序目录中的运行组件，并保留原数据、位置设置及 Windows 账户。`r`n新包不含数据或凭据；原有长期授权沿用现有 v3 格式。不要为尝试新界面清空授权或修改隧道。`r`n按完整路径排除应用的环境应沿用原固定 WinShellBot.exe 路径。`r`n详细操作与待验范围见 docs/USER_GUIDE.md。`r`n",$utf8)
WriteJson (Join-Path $stage 'manifest.json') @{stage='M20';version='0.1.0-dev.27';kind='native-ui-test';default_entry='ShellBot.exe';source_commit=$commit;source_local_changes=$true;pushed=$false;tools=37;compiler_language='C17';bundled_data=$false;bundled_credentials=$false;artifacts=$artifacts.Release;acceptance_sha256=(Digest (Join-Path $validation 'acceptance.json'));pending=$acceptance.pending;license_status='Third-party notices included; existing project copyright placeholder remains before public release'}
$hashes=@(Get-ChildItem -LiteralPath $stage -Recurse -File|Sort-Object FullName|ForEach-Object {@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');sha256=(Digest $_.FullName)}})
if($hashes.path -match '(^|/)(Data|tests|config|Runtime)(/|$)|\.(cmd|bat|ps1|py|pdb|sbproj|sbkey|dpapi)$|TestFixture|SettingsUiHost|ShellBot\.storage\.json'){throw 'Unexpected runtime data or development content in package.'}
if(@(Get-ChildItem -LiteralPath (Join-Path $stage 'licenses\pdfium') -File).Count -ne 17){throw 'Incomplete pinned PDFium notices.'}
WriteJson (Join-Path $stage 'SHA256SUMS.json') $hashes
# Verify inputs again before committing the archive.
foreach($row in $source){if((Digest (Join-Path $root $row.path)) -ne $row.sha256){throw 'Source changed during packaging.'}}
foreach($row in $tests){if((Digest (Join-Path $root ('tests\'+$row.name))) -ne $row.sha256){throw 'Test changed during packaging.'}}
foreach($row in $reports){if((Digest $row.path) -ne $row.sha256){throw 'Report changed during packaging.'}}
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$archiveHash=Digest $archive
[IO.File]::WriteAllText(($archive+'.sha256'),"$archiveHash  $name.zip`n",$utf8)
$receipt=@{stage='M20';status='packaged_pending_extracted_ui';archive=$archive;archive_sha256=$archiveHash;directory=$stage;source_commit=$commit;pushed=$false;files=$hashes.Count+1;acceptance_sha256=(Digest (Join-Path $validation 'acceptance.json'));executable_sha256=$artifacts.Release['WinShellBot.exe'].sha256}
WriteJson (Join-Path $validation 'package-receipt.json') $receipt
Write-Output "Package: $archive"
Write-Output "SHA256: $archiveHash"
