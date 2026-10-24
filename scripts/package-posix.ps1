param(
    [Parameter(Mandatory)][ValidatePattern('^\d{8}-\d{6}-[a-f0-9]{8}$')][string]$Run,
    [ValidateSet('all','mac','linux')][string]$Target='all',
    [ValidatePattern('^[a-z][a-z0-9-]{0,47}$')][string]$BuildVariant='build',
    [string]$ConnectionFile,
    [string]$SshDirectory='C:/Program Files/Git/usr/bin',
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$PSNativeCommandArgumentPassing='Standard'
if($PSVersionTable.PSVersion -lt [version]'7.3'){throw 'Use PowerShell 7.3 or newer.'}
$repo=Split-Path -Parent $PSScriptRoot
if(!$ConnectionFile){$ConnectionFile=Join-Path $repo 'build/remote-dev-20261022/connections.json'}
$inputDir=Join-Path $repo ('build/posix/'+$Run)
$receipt=Get-Content (Join-Path $inputDir 'receipt.json') -Raw|ConvertFrom-Json
if($receipt.run_id -cne $Run -or $receipt.source_commit -notmatch '^[a-f0-9]{40}$'){throw 'Source provenance is missing from the build receipt.'}
foreach($line in Get-Content (Join-Path $inputDir 'SOURCE_SHA256SUMS')){
    $digest,$name=$line -split '  ',2
    if((Get-FileHash -LiteralPath (Join-Path $repo $name) -Algorithm SHA256).Hash -ine $digest){throw "Source changed after the native snapshot: $name"}
}
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo ('build/posix/delivery-phase17-'+$Run)}
New-Item -ItemType Directory -Path $OutputDirectory -Force|Out-Null
function Q([string]$s){"'"+$s.Replace("'","'\''")+"'"}
$ssh=Join-Path $SshDirectory 'ssh.exe';$scp=Join-Path $SshDirectory 'scp.exe'
$rows=@()
foreach($c in Get-Content $ConnectionFile -Raw|ConvertFrom-Json){
    if($Target -ne 'all' -and $Target -ne $c.name){continue}
    if($c.name -notin @('mac','linux') -or $c.hostName -notmatch '^[-a-zA-Z0-9_.@]+$' -or $c.remoteRoot -notmatch '^/' -or $c.remoteRoot -match '[\r\n]'){throw 'Invalid connection record.'}
    if(Test-Path -LiteralPath (Join-Path $OutputDirectory ($c.name+'-package-receipt.json'))){throw 'Choose a new output directory; this target already has a candidate receipt.'}
    $snapshot=$c.remoteRoot+'/.native-build/'+$Run
    $relative='candidate-'+[guid]::NewGuid().ToString('N').Substring(0,12)
    $out=$snapshot+'/'+$relative
    $script=@'
set -eu
PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
export PATH
cd __SNAPSHOT__
python3 src/PosixShellBot/package.py --build-dir "$PWD/__VARIANT__" --output-dir __OUTPUT__ --source-revision __COMMIT__ --source-dirty __DIRTY__
'@
    $script=$script.Replace('__SNAPSHOT__',(Q $snapshot)).Replace('__VARIANT__',$BuildVariant).Replace('__OUTPUT__',(Q $out)).Replace('__COMMIT__',(Q $receipt.source_commit)).Replace('__DIRTY__',([string][bool]$receipt.source_dirty).ToLowerInvariant()).Replace("`r`n","`n")
    $local=Join-Path $OutputDirectory ($c.name+'-package.sh');[IO.File]::WriteAllText($local,$script,[Text.UTF8Encoding]::new($false))
    $options=@('-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o',"UserKnownHostsFile=$($c.knownHosts)",'-o','IdentitiesOnly=yes','-o','IdentityAgent=none','-o','ForwardAgent=no','-o','ClearAllForwardings=yes','-o','ConnectTimeout=10','-i',$c.identityFile)
    $remote=$snapshot+'/'+$relative+'.sh'
    & $scp @options $local ($c.hostName+':'+$remote);if($LASTEXITCODE){throw 'Transfer package script failed.'}
    & $ssh @options -T $c.hostName ('sh -n '+(Q $remote)+' && sh '+(Q $remote)) 2>&1|Tee-Object (Join-Path $OutputDirectory ($c.name+'-package.log'))
    if($LASTEXITCODE){throw "Candidate packaging failed on $($c.name); partial output preserved."}
    $proofFile=Join-Path $OutputDirectory ($c.name+'-package-receipt.json')
    & $scp @options ($c.hostName+':'+$out+'/package-receipt.json') $proofFile;if($LASTEXITCODE){throw 'Cannot retrieve native receipt.'}
    $proof=Get-Content $proofFile -Raw|ConvertFrom-Json
    if($proof.status -cne 'passed' -or $proof.package -notmatch '^ShellBot-[0-9.]+-posix\.[0-9]+-candidate-(macos|linux)-x64\.tar\.gz$' -or $proof.payload_directory -cne $proof.package.Replace('.tar.gz','') -or $proof.source.manifest_sha256 -cne (Get-FileHash (Join-Path $inputDir 'SOURCE_SHA256SUMS')).Hash.ToLowerInvariant()){throw 'Invalid or stale candidate receipt.'}
    foreach($name in @($proof.package,($proof.package+'.sha256'))){
        & $scp @options ($c.hostName+':'+$out+'/'+$name) (Join-Path $OutputDirectory $name);if($LASTEXITCODE){throw 'Cannot retrieve candidate archive.'}
    }
    & $scp @options ($c.hostName+':'+$out+'/'+$proof.payload_directory+'/package-manifest.json') (Join-Path $OutputDirectory ($c.name+'-package-manifest.json'));if($LASTEXITCODE){throw 'Cannot retrieve candidate manifest.'}
    if((Get-FileHash (Join-Path $OutputDirectory $proof.package) -Algorithm SHA256).Hash -ine $proof.sha256){throw 'Downloaded archive checksum mismatch.'}
    & python (Join-Path $repo 'PosixShellBot/distribution/verify-package.py') --archive (Join-Path $OutputDirectory $proof.package)
    if($LASTEXITCODE){throw 'Downloaded archive structure or manifest verification failed.'}
    $proof|Add-Member -NotePropertyName remote_output -NotePropertyValue $out -Force
    $proof|ConvertTo-Json -Depth 10|Set-Content $proofFile -Encoding utf8NoBOM
    $rows+=$proof
}
if(!$rows.Count){throw 'No target selected.'}
$rows|ConvertTo-Json -Depth 10|Set-Content (Join-Path $OutputDirectory ('packages-'+$Target+'.json')) -Encoding utf8NoBOM
Write-Host "Verified acceptance candidates: $OutputDirectory"
