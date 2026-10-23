param(
    [ValidateSet('all','mac','linux')][string]$Target='all',
    [string]$ConnectionFile,
    [string]$SshDirectory,
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [switch]$WithTests,
    [switch]$Sanitizers,
    [ValidateSet('all','project-profile','administrator-media','administrator-broker','administrator-ui','service-ui','remote-desktop','command-identities')][string]$TestScope='all'
)
$ErrorActionPreference='Stop'
if($TestScope -ne 'all' -and !$WithTests){throw 'TestScope requires -WithTests.'}
if($PSVersionTable.PSVersion -lt [version]'7.3'){throw 'Use PowerShell 7.3 or newer for native SSH argument handling.'}
$PSNativeCommandArgumentPassing='Standard'
$PSNativeCommandUseErrorActionPreference=$false
$repo=Split-Path -Parent $PSScriptRoot
if(!$ConnectionFile){$ConnectionFile=Join-Path $repo 'build\remote-dev-20261022\connections.json'}
$connections=Get-Content -LiteralPath $ConnectionFile -Raw | ConvertFrom-Json
$runId=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$runDir=Join-Path $repo "build\posix\$runId"
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$files=@('CMakeLists.txt','LICENSE.txt','third_party/yyjson/LICENSE','third_party/civetweb/LICENSE.md','docs/POSIX_BACKEND.md','docs/POSIX_OAUTH.md','config/posix-projects.example.json','config/posix-commands.example.json','config/posix-oauth.example.json')
foreach($folder in @('core','PosixShellBot','third_party/yyjson','third_party/civetweb/src','third_party/civetweb/include')){
    foreach($item in Get-ChildItem -LiteralPath (Join-Path $repo $folder) -Recurse -File){
        if($item.Extension -in @('.c','.h','.inl','.m','.plist','.desktop','.cmake')){$files+=[IO.Path]::GetRelativePath($repo,$item.FullName).Replace('\','/')}
    }
}
$files+='assets/branding/shellbot-v1/shellbot-icon.png';$files+='assets/branding/shellbot-v1/shellbot-icon-small.png'
$files+='docs/POSIX_DESKTOP.md'
$files+='docs/POSIX_REMOTE_DESKTOP.md';$files+='config/posix-desktop.example.json'
if($WithTests){$files+='tests/posix_integration.py';$files+='tests/posix_phase2.py';$files+='tests/posix_oauth.py';$files+='tests/posix_clock.c';$files+='tests/posix_desktop.py';$files+='tests/posix_tray.py'}
if($WithTests){$files+='tests/posix_remote.py';$files+='tests/posix_remote_fixture.c';$files+='tests/posix_desktop_contract.c'}
$files+='third_party/stb/stb_image.h';$files+='third_party/stb/LICENSE';$files+='third_party/stb/PROVENANCE.json'
$files+='third_party/pdfium/include/fpdfview.h';$files+='third_party/pdfium/include/fpdf_text.h'
foreach($platform in @('linux-x64','mac-x64')){foreach($item in Get-ChildItem -LiteralPath (Join-Path $repo ('third_party/pdfium/'+$platform)) -Recurse -File){$files+=[IO.Path]::GetRelativePath($repo,$item.FullName).Replace('\','/')}}
$files+='docs/POSIX_STARTUP.md';$files+='docs/POSIX_MEDIA.md';$files+='config/posix-media.example.json'
if($WithTests){$files+='tests/posix_media.py';$files+='tests/posix_startup.py'}
$files+='docs/POSIX_COMMANDS.md'
$files+='docs/POSIX_PROJECTS.md'
$files+='docs/POSIX_ACCEPTANCE.md'
$files+='config/posix-restricted.example.json';$files+='config/posix-admin.example.json'
if($WithTests){$files+='tests/posix_commands.py';$files+='tests/posix_command_fixture.c'}
if($WithTests){$files+='tests/posix_profile.py'}
if($WithTests){$files+='tests/posix_admin_contract.c';$files+='tests/posix_admin_media.py'}
if($WithTests){$files+='tests/posix_broker_contract.c';$files+='tests/posix_broker.py';$files+='tests/posix_admin_ui_contract.c';$files+='tests/posix_admin_ui.py';$files+='tests/posix_service_ui.py';$files+='tests/posix_service_contract.c'}
$files=@($files|Sort-Object -Unique)
$hashes=foreach($file in $files){$path=Join-Path $repo $file;if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw "Missing input: $file"};((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+$file)}
[IO.File]::WriteAllText((Join-Path $runDir 'SOURCE_SHA256SUMS'),($hashes -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
$fileList=Join-Path $runDir 'files.txt'
[IO.File]::WriteAllText($fileList,($files -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
$archive=Join-Path $runDir 'source.tar'
& tar -cf $archive -C $repo -T $fileList
if($LASTEXITCODE -ne 0){throw 'Cannot archive selected source inputs.'}
& tar -rf $archive -C $runDir SOURCE_SHA256SUMS
if($LASTEXITCODE -ne 0){throw 'Cannot append source verification manifest.'}
function Quote-Sh([string]$Text){return "'"+$Text.Replace("'","'\''")+"'"}
if(!$SshDirectory){$SshDirectory=Join-Path $env:WINDIR 'System32\OpenSSH'}
$ssh=Join-Path $SshDirectory 'ssh.exe'
$scp=Join-Path $SshDirectory 'scp.exe'
if(!(Test-Path -LiteralPath $ssh -PathType Leaf) -or !(Test-Path -LiteralPath $scp -PathType Leaf)){throw 'SshDirectory must contain ssh.exe and scp.exe.'}
$results=@()
foreach($connection in $connections){
    if($Target -ne 'all' -and $Target -ne $connection.name){continue}
    if($connection.name -notin @('mac','linux') -or $connection.hostName -notmatch '^[-a-zA-Z0-9_.@]+$' -or $connection.remoteRoot -notmatch '^/' -or $connection.remoteRoot.Contains("`n")){throw 'Invalid local connection record.'}
    $options=@('-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o',"UserKnownHostsFile=$($connection.knownHosts)",'-o','IdentitiesOnly=yes','-o','IdentityAgent=none','-o','ForwardAgent=no','-o','ClearAllForwardings=yes','-o','ConnectTimeout=10','-o','ServerAliveInterval=15','-o','ServerAliveCountMax=2','-i',$connection.identityFile)
    $root=Quote-Sh $connection.remoteRoot
    $snapshot="$($connection.remoteRoot)/.native-build/$runId"
    $snapshotQuoted=Quote-Sh $snapshot
    $prepare="set -eu`ncd $root`ntest `"`$(pwd -P)`" = $root`nmkdir -p .native-build`nmkdir $snapshotQuoted`n"
    & $ssh @options -T $connection.hostName ('sh -c '+(Quote-Sh $prepare))
    if($LASTEXITCODE -ne 0){throw "Cannot prepare an isolated build directory on $($connection.name)."}
    & $scp @options $archive ($connection.hostName+':'+$snapshot+'/source.tar')
    if($LASTEXITCODE -ne 0){throw "Cannot transfer source snapshot to $($connection.name)."}
    $testFlag=if($WithTests){'ON'}else{'OFF'}
    $sanitizerFlag=if($Sanitizers){'ON'}else{'OFF'}
    $remote=@'
set -eu
PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
export PATH
cd __SNAPSHOT__
mkdir src
tar -xf source.tar -C src
cd src
if command -v sha256sum >/dev/null 2>&1; then sha256sum -c SOURCE_SHA256SUMS > ../source-verification.log; else shasum -a 256 -c SOURCE_SHA256SUMS > ../source-verification.log; fi
cd ..
if cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=__CONFIG__ -DSHELLBOT_TESTS=__TESTS__ -DSHELLBOT_SANITIZERS=__SANITIZERS__ > configure.log 2>&1; then cat configure.log; else cat configure.log; exit 1; fi
if cmake --build build --parallel 4 > build.log 2>&1; then cat build.log; else cat build.log; exit 1; fi
build/ShellBot --version
if [ __TESTS__ = ON ]; then
    if ctest --test-dir build __TEST_SELECTION__ --output-on-failure > test.log 2>&1; then cat test.log; else cat test.log; exit 1; fi
fi
'@
    $selection=if($TestScope -eq 'all'){''}else{"-R '^$TestScope$'"}
    $remote=$remote.Replace('__SNAPSHOT__',$snapshotQuoted).Replace('__CONFIG__',$Configuration).Replace('__TESTS__',$testFlag).Replace('__SANITIZERS__',$sanitizerFlag).Replace('__TEST_SELECTION__',$selection).Replace("`r`n","`n")
    $log=Join-Path $runDir ($connection.name+'-build.log')
    & $ssh @options -T $connection.hostName ('sh -c '+(Quote-Sh $remote)) 2>&1 | Tee-Object -FilePath $log
    $buildExit=$LASTEXITCODE
    & $scp @options ($connection.hostName+':'+$snapshot+'/build/ShellBot') (Join-Path $runDir ($connection.name+'-ShellBot'))
    $artifactExit=$LASTEXITCODE
    $testVerified=$null
    if($WithTests -and $TestScope -ne 'all'){
        $reportKind=@{'project-profile'='profile';'administrator-media'='admin-media';'administrator-broker'='admin-broker';'administrator-ui'='admin-ui';'service-ui'='service-ui';'remote-desktop'='remote-desktop';'command-identities'='commands'}[$TestScope]
        $profileReport=Join-Path $runDir ($connection.name+'-'+$reportKind+'-report.json')
        & $scp @options ($connection.hostName+':'+$snapshot+'/build/'+$reportKind+'-test-report.json') $profileReport
        $testVerified=$false
        if($LASTEXITCODE -eq 0 -and $artifactExit -eq 0){
            $profileResult=Get-Content -LiteralPath $profileReport -Raw|ConvertFrom-Json
            $testVerified=$profileResult.status -ceq 'passed' -and $profileResult.sha256 -ceq (Get-FileHash -LiteralPath (Join-Path $runDir ($connection.name+'-ShellBot')) -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    if($WithTests -and $TestScope -eq 'all'){
        $testReport=Join-Path $runDir ($connection.name+'-test-report.json')
        & $scp @options ($connection.hostName+':'+$snapshot+'/build/posix-test-report.json') $testReport
        $testVerified=$false
        if($LASTEXITCODE -eq 0 -and $artifactExit -eq 0){
            $testResult=Get-Content -LiteralPath $testReport -Raw | ConvertFrom-Json
            $testVerified=$testResult.status -ceq 'passed' -and $testResult.sha256 -ceq (Get-FileHash -LiteralPath (Join-Path $runDir ($connection.name+'-ShellBot')) -Algorithm SHA256).Hash.ToLowerInvariant()
            $desktopReport=Join-Path $runDir ($connection.name+'-desktop-report.json')
            $profileReport=Join-Path $runDir ($connection.name+'-profile-report.json')
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/profile-test-report.json') $profileReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $profileResult=Get-Content -LiteralPath $profileReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $profileResult.status -ceq 'passed' -and $profileResult.sha256 -ceq $testResult.sha256
            }
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/desktop-test-report.json') $desktopReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $desktopResult=Get-Content -LiteralPath $desktopReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $desktopResult.status -ceq 'passed' -and $desktopResult.sha256 -ceq $testResult.sha256
            }
            $startupReport=Join-Path $runDir ($connection.name+'-startup-report.json')
            $commandsReport=Join-Path $runDir ($connection.name+'-commands-report.json')
            $adminReport=Join-Path $runDir ($connection.name+'-admin-media-report.json')
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/admin-media-test-report.json') $adminReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $adminResult=Get-Content -LiteralPath $adminReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $adminResult.status -ceq 'passed' -and $adminResult.sha256 -ceq $testResult.sha256
            }
            $brokerReport=Join-Path $runDir ($connection.name+'-admin-broker-report.json')
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/admin-broker-test-report.json') $brokerReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $brokerResult=Get-Content -LiteralPath $brokerReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $brokerResult.status -ceq 'passed' -and $brokerResult.sha256 -ceq $testResult.sha256
            }
            $uiReport=Join-Path $runDir ($connection.name+'-admin-ui-report.json')
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/admin-ui-test-report.json') $uiReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $uiResult=Get-Content -LiteralPath $uiReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $uiResult.status -ceq 'passed' -and $uiResult.sha256 -ceq $testResult.sha256
            }
            $serviceReport=Join-Path $runDir ($connection.name+'-service-ui-report.json')
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/service-ui-test-report.json') $serviceReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $serviceResult=Get-Content -LiteralPath $serviceReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $serviceResult.status -ceq 'passed' -and $serviceResult.sha256 -ceq $testResult.sha256
            }
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/commands-test-report.json') $commandsReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $commandsResult=Get-Content -LiteralPath $commandsReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $commandsResult.status -ceq 'passed' -and $commandsResult.sha256 -ceq $testResult.sha256
            }
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/startup-test-report.json') $startupReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $startupResult=Get-Content -LiteralPath $startupReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $startupResult.status -ceq 'passed' -and $startupResult.sha256 -ceq $testResult.sha256
            }
            $mediaReport=Join-Path $runDir ($connection.name+'-media-report.json')
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/media-test-report.json') $mediaReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $mediaResult=Get-Content -LiteralPath $mediaReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $mediaResult.status -ceq 'passed' -and $mediaResult.sha256 -ceq $testResult.sha256
            }
            $remoteReport=Join-Path $runDir ($connection.name+'-remote-desktop-report.json')
            & $scp @options ($connection.hostName+':'+$snapshot+'/build/remote-desktop-test-report.json') $remoteReport
            if($LASTEXITCODE -ne 0){$testVerified=$false}else{
                $remoteResult=Get-Content -LiteralPath $remoteReport -Raw|ConvertFrom-Json
                $testVerified=$testVerified -and $remoteResult.status -ceq 'passed' -and $remoteResult.sha256 -ceq $testResult.sha256
            }
        }
    }
    $results+=[ordered]@{target=$connection.name;remote_snapshot=$snapshot;build_exit=$buildExit;artifact_copied=($artifactExit -eq 0);tests_requested=[bool]$WithTests;test_scope=$TestScope;test_report_verified=$testVerified;configuration=$Configuration;sanitizers=[bool]$Sanitizers}
}
$receipt=[ordered]@{run_id=$runId;source_archive_sha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash;input_count=$files.Count;results=$results}
$receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir 'receipt.json') -Encoding utf8
Write-Host "Build records: $runDir"
if(!$results.Count -or @($results|Where-Object {$_.build_exit -ne 0 -or !$_.artifact_copied -or ($_.tests_requested -and !$_.test_report_verified)}).Count){throw 'One or more native builds failed; see saved logs and receipt.'}
