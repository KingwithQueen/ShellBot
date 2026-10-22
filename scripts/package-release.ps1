param(
    [string]$ValidationDirectory,
    [string]$OutputDirectory,
    [ValidatePattern('^$|^[a-z0-9][a-z0-9-]{0,39}$')][string]$PackageLabel=''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$ValidationDirectory){
    $header=Get-Content (Join-Path $root 'core\include\sb_version.h') -Raw
    if($header -notmatch '(?m)^#define SB_VERSION "(\d+\.\d+\.\d+)"\s*$'){throw 'Stable version is required.'}
    $ValidationDirectory=Join-Path $root ('build\release-'+$Matches[1])
}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'dist'}
$validation=[IO.Path]::GetFullPath($ValidationDirectory)
$destination=[IO.Path]::GetFullPath($OutputDirectory)
$utf8=[Text.UTF8Encoding]::new($false)
function Digest([string]$path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
function WriteJson([string]$path,$value){[IO.File]::WriteAllText($path,($value|ConvertTo-Json -Depth 14),$utf8)}
function Require($condition,[string]$message){if(!$condition){throw $message}}
$versionHeader=Get-Content (Join-Path $root 'core\include\sb_version.h') -Raw
Require ($versionHeader -match '(?m)^#define SB_VERSION "(\d+\.\d+\.\d+)"\s*$') 'Stable version is required.'
$version=$Matches[1]
$releaseNotes="RELEASE_NOTES_$version.md"
Require (Test-Path -LiteralPath (Join-Path $root ('docs\'+$releaseNotes))) 'Current release notes are required.'
$runtime=@('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')
$artifacts=[ordered]@{};$reports=@()
$stages=@{'layout'='M21-layout';'refresh'='M14-manager-refresh';'native-ui'='M20-native-ui';'oauth'='M15-OAuth';'native-dialogs'='M20-native-dialogs';'desktop'='M14-desktop';'entry'='M14-entry';'exit'='M14-exit';'oauth-ui'='M15-OAuth-UI';'portable'='M16-portable';'files'='M13-files';'settings'='M13-settings';'images'='M9';'documents'='M10';'document-backend'='M10_PDF_dependency';'transfers'='M11';'gateway'='M3';'eof'='HTTP-EOF';'single-tray'='single-tray';'folder-picker'='folder-picker'}
foreach($configuration in @('Debug','Release')){
    $binaries=[ordered]@{}
    foreach($name in $runtime){
        $path=Join-Path $root "out\x64\$configuration\$name"
        $binaries[$name]=@{sha256=(Digest $path);bytes=(Get-Item -LiteralPath $path).Length}
        if($name -ne 'pdfium.dll'){
            $info=(Get-Item -LiteralPath $path).VersionInfo
            Require ($info.FileVersion -ceq $version -and $info.ProductVersion -ceq $version -and $info.ProductName -ceq 'ShellBot') "Incorrect Windows version: $path"
            Require ($info.LegalCopyright -ceq 'Copyright (c) 2026 姬敬') "Incorrect copyright: $path"
        }
    }
    $artifacts[$configuration]=$binaries
    $names=@('core','layout','refresh','native-ui','oauth','single-tray','folder-picker')
    if($configuration -eq 'Release'){$names+=@('native-dialogs','desktop','entry','exit','oauth-ui','portable','files','settings','images','documents','document-backend','transfers','project','gateway','eof')}
    foreach($name in $names){
        $stem=$configuration.ToLower()+'-'+$name
        $path=Join-Path $validation ($stem+'.json');$binding=Get-Content (Join-Path $validation ($stem+'-binding.json')) -Raw|ConvertFrom-Json
        $value=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
        $hash=if($value.sha256){$value.sha256}elseif($value.artifact.sha256){$value.artifact.sha256}else{$value.exe_sha256}
        Require (($value.passed -ceq $true -or $value.status -ceq 'passed') -and ($null -eq $value.passed -or $value.passed -ceq $true) -and (!$value.status -or $value.status -ceq 'passed')) "Failed report: $path"
        Require ($hash -ceq $binaries['WinShellBot.exe'].sha256) "Stale report: $path"
        Require ($binding.name -ceq $name -and $binding.configuration -ceq $configuration -and $binding.passed -ceq $true -and $binding.report_sha256 -ceq (Digest $path) -and $binding.executable_sha256 -ceq $hash) "Incorrect report binding: $path"
        Require ($binding.test -match '^[a-z_]+\.py$' -and $binding.test_sha256 -ceq (Digest (Join-Path $root ('tests\'+$binding.test)))) "Test changed after run: $path"
        if($stages[$name]){Require ($value.stage -ceq $stages[$name]) "Wrong stage: $path"}
        if($value.checks -is [array]){Require (@($value.checks|Where-Object {$_.passed -cne $true}).Count -eq 0) "Failed nested check: $path"}
        if($name -eq 'core'){Require ($value.scope -ceq 'full' -and $value.close_probe.calls -eq 100 -and $value.close_probe.failures -eq 0) 'Full core and connection close checks required.'}
        if($name -eq 'oauth'){Require ($value.long_term_authorization_verified -ceq $true -and $value.storage_format -eq 3 -and ($configuration -ne 'Release' -or $value.real_code_expiry_verified -ceq $true)) 'Persistent OAuth and real code expiry required.'}
        if($name -eq 'entry'){Require ($value.entry_sha256 -ceq $binaries['ShellBot.exe'].sha256) 'Launcher report is stale.'}
        if($name -eq 'project'){Require ($value.scope -ceq 'M1_configuration_policy_and_migration' -and $value.policy_test_artifact.sha256 -ceq (Digest (Join-Path $root 'out\x64\Release\ShellBot-ProjectPolicyTests.exe'))) 'Policy test artifact mismatch.'}
        if($name -eq 'eof'){Require ($value.clean_exit -ceq $true -and @($value.groups).Count -eq 4 -and @($value.failures).Count -eq 0 -and @($value.groups|Where-Object {$_.calls -lt 100 -or $_.failures -ne 0}).Count -eq 0) 'Strict EOF evidence required.'}
        if($name -eq 'folder-picker'){
            Require ($value.assertions -eq @($value.checks).Count -and $value.assertions -ge 30) 'Complete folder picker evidence required.'
            foreach($required in @('portable Desktop exists before Shell initialization','new project picker enumerates real child directories','project selection returns complete Unicode filesystem path','custom workspace survives storage selection','permissions returns complete Unicode filesystem path','application storage returns complete Unicode filesystem path','application exits after repeated Shell dialog use')){
                Require (@($value.checks|Where-Object {$_.name -ceq $required -and $_.passed -ceq $true}).Count -eq 1) "Missing folder picker evidence: $required"
            }
        }
        if($name -eq 'single-tray'){
            Require ($value.assertions -eq @($value.checks).Count -and $value.assertions -ge 33) 'Complete single-tray integration evidence required.'
            foreach($required in @('two real desktop workers have no notification icons','tray desktop pause preserves all project workers','confirmed resume succeeds through real owner channel after safety delay','client can renew a valid desktop lease','one session remains exclusive across projects after removing worker icons','lease expires while manager window is hidden and workers have no tray icons','taskbar recovery recreates only the manager icon','tray exit removes every project desktop backend')){
                Require (@($value.checks|Where-Object {$_.name -ceq $required -and $_.passed -ceq $true}).Count -eq 1) "Missing single-tray evidence: $required"
            }
        }
        $count=if($value.assertions){$value.assertions}elseif($value.checks_count){$value.checks_count}elseif($value.assertions_passed){$value.assertions_passed}elseif($name -eq 'eof'){($value.groups|Measure-Object calls -Sum).Sum}else{@($value.checks).Count}
        $reports+=@{configuration=$configuration;name=$name;passed=$true;checks=$count;sha256=(Digest $path);report=$stem+'.json';executable_sha256=$hash;test=$binding.test;test_sha256=$binding.test_sha256}
    }
}
$releaseExe=Join-Path $root 'out\x64\Release\WinShellBot.exe'
$displayVersion=& $releaseExe --version
Require ($LASTEXITCODE -eq 0 -and $displayVersion -ceq "ShellBot $version (C17)") 'Incorrect program version.'
$license=Get-Content (Join-Path $root 'LICENSE.txt') -Raw
Require ($license.Contains('Copyright (c) 2026 姬敬') -and $license -notmatch '\[year\]|\[fullname\]') 'Unfinished project license.'
$iconPath=Join-Path $validation 'icon-verification.json'
$icons=Get-Content -LiteralPath $iconPath -Raw|ConvertFrom-Json
Require ($icons.passed -ceq $true -and $icons.rgba_transparency_verified -ceq $true -and $icons.native_decoded_frames -eq 60 -and @($icons.components).Count -eq 6 -and (@($icons.sizes) -join ',') -ceq '16,20,24,32,40,48,64,96,128,256') 'Complete native icon verification required.'
Require ($icons.source_png_sha256 -ceq (Digest (Join-Path $root 'assets\branding\shellbot-v1\shellbot-icon.png')) -and $icons.ico_sha256 -ceq (Digest (Join-Path $root 'assets\branding\shellbot-v1\shellbot.ico'))) 'Icon source changed after verification.'
foreach($configuration in @('Debug','Release')){foreach($name in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe')){
    $iconMatches=@($icons.components|Where-Object {$_.configuration -ceq $configuration -and $_.name -ceq $name})
    Require ($iconMatches.Count -eq 1 -and $iconMatches[0].sha256 -ceq $artifacts[$configuration][$name].sha256 -and $iconMatches[0].embedded_frames -eq 10 -and $iconMatches[0].native_decode -ceq $true) 'Stale embedded icon verification.'
}}
$commit=(& git -C $root rev-parse HEAD).Trim()
$localChanges=@(& git -C $root status --porcelain --untracked-files=normal)
Require ($LASTEXITCODE -eq 0) 'Cannot inspect source status.'
$sourceLocalChanges=$localChanges.Count -ne 0
$paths=@(& git -C $root -c core.quotepath=false ls-files --cached --others --exclude-standard|Sort-Object -Unique)
Require ($LASTEXITCODE -eq 0) 'Cannot inventory source.'
$source=@($paths|ForEach-Object {@{path=$_;sha256=(Digest (Join-Path $root $_))}})
$tests=@(Get-ChildItem -LiteralPath (Join-Path $root 'tests') -File|Where-Object {$_.Extension -in @('.py','.c','.h','.vcxproj')}|Sort-Object Name|ForEach-Object {@{name=$_.Name;sha256=(Digest $_.FullName)}})
WriteJson (Join-Path $validation 'source-manifest.json') @{source_commit=$commit;files=$source}
WriteJson (Join-Path $validation 'local-test-manifest.json') @{files=$tests}
$acceptance=@{version=$version;kind='stable';status='local_regression_passed';source_commit=$commit;source_local_changes=$sourceLocalChanges;pushed=$false;artifacts=$artifacts;reports=$reports;report_count=$reports.Count;source_manifest_sha256=(Digest (Join-Path $validation 'source-manifest.json'));test_manifest_sha256=(Digest (Join-Path $validation 'local-test-manifest.json'));environment=@{adguard='Running; existing fixed-path exclusions; localhost filtering retained';temporary_files='E: workspace';network_runtime='out/x64/Debug and out/x64/Release';real_data_used=$false};pending=@('real_client_continuous_refresh','fresh_administrator_uac','physical_multi_monitor_dpi','independent_windows_systems','long_running_acceptance')}
$acceptance.icons=@{source_png_sha256=$icons.source_png_sha256;ico_sha256=$icons.ico_sha256;sizes=$icons.sizes;native_decoded_frames=$icons.native_decoded_frames;verification_sha256=(Digest $iconPath)}
WriteJson (Join-Path $validation 'acceptance.json') $acceptance
$name="ShellBot-$version-windows-x64"
if($PackageLabel){$name+='-'+$PackageLabel}
$stage=Join-Path $destination $name;$archive=$stage+'.zip'
Require (!(Test-Path -LiteralPath $stage) -and !(Test-Path -LiteralPath $archive) -and !(Test-Path -LiteralPath ($archive+'.sha256'))) 'Release destination already exists.'
$files=[ordered]@{}
foreach($file in $runtime){$files[$file]=Join-Path $root ('out\x64\Release\'+$file)}
$files['LICENSE.txt']=Join-Path $root 'LICENSE.txt'
foreach($file in @('USER_GUIDE.md','RELEASE_NOTES_1.0.0.md',$releaseNotes,'FILE_MANAGEMENT.md','FILE_EDIT.md','EXECUTION_PERMISSIONS.md','COMMAND_TASKS.md','REMOTE_APPROVALS.md','DESKTOP_CONTROL.md','IMAGE_VIEW.md','DOCUMENT_READING.md','ARTIFACT_TRANSFER.md','AUDIT.md','PROJECT_APPROVALS.md','SHARED_ADMINISTRATOR.md','STARTUP.md')){$files['docs/'+$file]=Join-Path $root ('docs\'+$file)}
$files['licenses/yyjson-MIT.txt']=Join-Path $root 'third_party\yyjson\LICENSE'
$files['licenses/CivetWeb-MIT.txt']=Join-Path $root 'third_party\civetweb\LICENSE.md'
$files['licenses/PDFium-binaries-MIT.txt']=Join-Path $root 'third_party\pdfium\LICENSE'
$files['licenses/PDFium-provenance.json']=Join-Path $root 'third_party\pdfium\provenance.json'
foreach($file in Get-ChildItem -LiteralPath (Join-Path $root 'third_party\pdfium\licenses') -File){$files['licenses/pdfium/'+$file.Name]=$file.FullName}
Require (@($files.Keys|Where-Object {$_.StartsWith('licenses/pdfium/')}).Count -eq 17) 'Incomplete PDFium notices.'
foreach($file in @('acceptance.json','source-manifest.json','local-test-manifest.json','icon-verification.json')){$files['validation/'+$file]=Join-Path $validation $file}
# All rejection gates above run before creating any distribution files.
foreach($entry in $files.GetEnumerator()){$target=Join-Path $stage $entry.Key;[IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null;Copy-Item -LiteralPath $entry.Value -Destination $target}
$readme="# ShellBot $version`n`nWindows x64 正式便携版。完整解压到可写的非 C 盘目录，双击 **ShellBot.exe**。在管理界面创建项目、设置权限并连接客户端。关闭窗口收进托盘，从菜单退出会停止全部项目。`n`n升级前先退出旧版，备份 Data 和 ShellBot.storage.json；替换程序时保留原数据、位置设置和 Windows 账户。`n`n操作见 [使用说明](docs/USER_GUIDE.md)，变化与准确验证范围见 [发布说明](docs/$releaseNotes)。校验清单为 SHA256SUMS.json，第三方声明位于 licenses。`n`nCopyright (c) 2026 姬敬。MIT 许可。`n"
[IO.File]::WriteAllText((Join-Path $stage 'README.md'),$readme,$utf8)
# Developer handoffs and source-only examples remain in the repository. Keep
# their labels as plain text instead of shipping broken local links.
foreach($document in Get-ChildItem -LiteralPath (Join-Path $stage 'docs') -Filter '*.md' -File){
    $text=[IO.File]::ReadAllText($document.FullName)
    $text=[regex]::Replace($text,'(?<!!)\[([^\]\r\n]+)\]\(([^)\r\n]+)\)',[Text.RegularExpressions.MatchEvaluator]{param($match)
        $link=$match.Groups[2].Value
        if($link -match '^[a-z][a-z0-9+.-]*:|^#'){return $match.Value}
        $linked=Join-Path $document.DirectoryName ($link.Split('#')[0])
        if(Test-Path -LiteralPath $linked){return $match.Value}
        return $match.Groups[1].Value
    })
    [IO.File]::WriteAllText($document.FullName,$text,$utf8)
}
WriteJson (Join-Path $stage 'manifest.json') @{version=$version;kind='stable';default_entry='ShellBot.exe';source_commit=$commit;source_local_changes=$sourceLocalChanges;pushed=$false;tools=37;compiler_language='C17';bundled_data=$false;bundled_credentials=$false;artifacts=$artifacts.Release;acceptance_sha256=(Digest (Join-Path $validation 'acceptance.json'));pending=$acceptance.pending;license='MIT; Copyright (c) 2026 姬敬'}
$hashes=@(Get-ChildItem -LiteralPath $stage -Recurse -File|Sort-Object FullName|ForEach-Object {@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');sha256=(Digest $_.FullName)}})
Require (!($hashes.path -match '(^|/)(Data|tests|config|Runtime)(/|$)|\.(cmd|bat|ps1|py|pdb|sbproj|sbkey|dpapi)$|TestFixture|SettingsUiHost|ShellBot\.storage\.json')) 'Unexpected private or development files.'
WriteJson (Join-Path $stage 'SHA256SUMS.json') $hashes
foreach($row in $source){Require ((Digest (Join-Path $root $row.path)) -ceq $row.sha256) 'Source changed during packaging.'}
foreach($row in $tests){Require ((Digest (Join-Path $root ('tests\'+$row.name))) -ceq $row.sha256) 'Test changed during packaging.'}
foreach($row in $reports){Require ((Digest (Join-Path $validation $row.report)) -ceq $row.sha256) 'Report changed during packaging.'}
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$archiveHash=Digest $archive
[IO.File]::WriteAllText(($archive+'.sha256'),"$archiveHash  $name.zip`n",$utf8)
WriteJson (Join-Path $validation 'package-receipt.json') @{version=$version;status='packaged_pending_extracted_runtime';archive=$archive;archive_sha256=$archiveHash;directory=$stage;source_commit=$commit;pushed=$false;files=$hashes.Count+1;acceptance_sha256=(Digest (Join-Path $validation 'acceptance.json'));executable_sha256=$artifacts.Release['WinShellBot.exe'].sha256}
Write-Output "Package: $archive"
Write-Output "SHA256: $archiveHash"
