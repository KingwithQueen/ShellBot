param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$folder=Join-Path $root 'build\native-ui-validation'
$env:TEMP=Join-Path $folder 'Temp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
$runner=Join-Path $root 'build\tools\ShellBotNetworkTest\ShellBot-TestClient.exe'
$exe=Join-Path $root "out\x64\$Configuration\WinShellBot.exe"
$cases=@(
    @{name='native-ui';file='native_ui_checks.py';extra=@()},
    @{name='native-dialogs';file='native_ui_dialog_checks.py';extra=@()},
    @{name='refresh';file='manager_refresh_checks.py';extra=@()},
    @{name='oauth-ui';file='oauth_ui_checks.py';extra=@()},
    @{name='oauth-refresh-ui';file='oauth_refresh_ui_checks.py';extra=@()},
    @{name='oauth';file='oauth_checks.py';extra=$(if($Configuration -eq 'Release'){@('--expiry')}else{@()})}
)
if($Configuration -eq 'Release'){
    $cases+=@(
        @{name='desktop';file='desktop_app_checks.py';extra=@()},
        @{name='entry';file='desktop_entry_checks.py';extra=@()},
        @{name='exit';file='desktop_exit_checks.py';extra=@()},
        @{name='settings';file='settings_ui_checks.py';extra=@('--product')},
        @{name='management';file='management_ui_checks.py';extra=@()},
        @{name='portable';file='portable_storage_checks.py';extra=@()},
        @{name='core';file='smoke.py';extra=@()}
    )
}
Push-Location $root
try{
    foreach($case in $cases){
        $prefix=$Configuration.ToLower()+'-'+$case.name
        $report=Join-Path $folder ($prefix+'.json');$log=Join-Path $folder ($prefix+'.log')
        if((Test-Path -LiteralPath $report) -or (Test-Path -LiteralPath $log)){
            $saved=Join-Path $folder ('preserved-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$prefix)
            New-Item -ItemType Directory -Path $saved | Out-Null
            foreach($old in @($report,$log)){if(Test-Path -LiteralPath $old){Copy-Item -LiteralPath $old -Destination $saved}}
        }
        Write-Output "START $prefix"
        $arguments=@((Join-Path $root ('tests\'+$case.file)),'--exe',$exe,'--report',$report)+$case.extra
        & $runner @arguments *> $log
        if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log -Tail 15;throw "Failed $prefix"}
        $value=Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
        $passed=$value.passed -ceq $true -or $value.status -ceq 'passed'
        $digest=if($value.sha256){$value.sha256}elseif($value.artifact.sha256){$value.artifact.sha256}else{$value.exe_sha256}
        if(!$passed -or $digest -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower()){throw "Report mismatch $prefix"}
        Write-Output "PASS $prefix"
    }
}finally{Pop-Location}
