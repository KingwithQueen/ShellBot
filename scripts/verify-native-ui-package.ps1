param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$validation=Join-Path $root 'build\native-ui-validation'
$receiptPath=Join-Path $validation 'package-receipt.json'
$receipt=Get-Content -LiteralPath $receiptPath -Raw -Encoding utf8|ConvertFrom-Json
function Digest([string]$path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
if($receipt.stage -cne 'M20' -or (Digest $receipt.archive) -ne $receipt.archive_sha256){throw 'Package receipt does not match archive.'}
$name=[IO.Path]::GetFileNameWithoutExtension($receipt.archive)
$folder=Join-Path $validation ('extracted-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
if(Test-Path -LiteralPath $folder){throw 'Extraction destination already exists.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($receipt.archive)
try{
    foreach($entry in $zip.Entries){
        $path=$entry.FullName.Replace('\','/')
        if(!$path.StartsWith($name+'/',[StringComparison]::Ordinal) -or $path -match '(^|/)\.\.(/|$)|:'){throw 'Unexpected archive path.'}
    }
}finally{$zip.Dispose()}
Expand-Archive -LiteralPath $receipt.archive -DestinationPath $folder
$package=Join-Path $folder $name
$sums=@(Get-Content -LiteralPath (Join-Path $package 'SHA256SUMS.json') -Raw -Encoding utf8|ConvertFrom-Json)
$checks=@()
foreach($row in $sums){
    if($row.path -match '(^|/)\.\.(/|$)|:|^[/\\]'){throw 'Unexpected manifest path.'}
    if((Digest (Join-Path $package $row.path)) -ne $row.sha256){throw 'Extracted content mismatch.'}
    $checks+=@{name=$row.path;passed=$true}
}
$expected=@($sums.path)+@('SHA256SUMS.json')
$actual=@(Get-ChildItem -LiteralPath $package -Recurse -File|ForEach-Object {$_.FullName.Substring($package.Length+1).Replace('\','/')})
if(Compare-Object @($expected|Sort-Object) @($actual|Sort-Object)){throw 'Unexpected extracted file set.'}
$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding utf8|ConvertFrom-Json
foreach($name in @('WinShellBot.exe','ShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')){
    if((Digest (Join-Path $package $name)) -ne $manifest.artifacts.$name.sha256){throw 'Runtime component does not match verified build.'}
}
$env:TEMP=Join-Path $validation 'Temp';$env:TMP=$env:TEMP
$runner=Join-Path $root 'build\tools\ShellBotNetworkTest\ShellBot-TestClient.exe'
$exe=Join-Path $package 'WinShellBot.exe';$reports=@()
Push-Location $root
try{
    foreach($case in @(@{name='native-ui';file='native_ui_checks.py'},@{name='native-dialogs';file='native_ui_dialog_checks.py'},@{name='entry';file='desktop_entry_checks.py'})){
        $report=Join-Path $validation ('extracted-'+$case.name+'.json');$log=Join-Path $validation ('extracted-'+$case.name+'.log')
        if((Test-Path -LiteralPath $report) -or (Test-Path -LiteralPath $log)){throw 'Extraction report destination already exists; preserve it before retrying.'}
        & $runner (Join-Path $root ('tests\'+$case.file)) --exe $exe --report $report *> $log
        if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log -Tail 12;throw 'Extracted runtime check failed.'}
        $value=Get-Content -LiteralPath $report -Raw -Encoding utf8|ConvertFrom-Json
        if($value.passed -cne $true -or $value.sha256 -ne $receipt.executable_sha256){throw 'Extracted report mismatch.'}
        $reports+=@{name=$case.name;path=$report;sha256=(Digest $report);assertions=$value.assertions;passed=$true}
    }
}finally{Pop-Location}
$after=@(Get-ChildItem -LiteralPath $package -Recurse -File|ForEach-Object {$_.FullName.Substring($package.Length+1).Replace('\','/')})
if(Compare-Object @($expected|Sort-Object) @($after|Sort-Object)){throw 'Runtime unexpectedly wrote inside the package.'}
foreach($row in $sums){if((Digest (Join-Path $package $row.path)) -ne $row.sha256){throw 'Package content changed after runtime checks.'}}
$remaining=@(Get-Process -Name WinShellBot -ErrorAction SilentlyContinue|Where-Object Path -eq $exe)
if($remaining.Count){throw 'Extracted test process remains active.'}
$result=@{stage='M20-extracted';passed=$true;archive=$receipt.archive;archive_sha256=$receipt.archive_sha256;directory=$package;sha256=$receipt.executable_sha256;files=$actual.Count;checks=$checks;reports=$reports;owned_processes_remaining=0;scope='real extracted files, native UI fault fixture, native dialogs and public entry; network stress at this unexcluded path not claimed'}
$output=Join-Path $validation 'extracted-runtime.json';$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($output,($result|ConvertTo-Json -Depth 8),$encoding)
$receipt.status='native_ui_verified_pending_external_acceptance';$receipt|Add-Member -NotePropertyName extracted_runtime -NotePropertyValue @{path=$output;sha256=(Digest $output)} -Force
[IO.File]::WriteAllText($receiptPath,($receipt|ConvertTo-Json -Depth 8),$encoding)
Write-Output "Extracted runtime passed: $package"
