param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Package,[Parameter(Mandatory=$true)][string]$Job,[int]$ParentId,[switch]$Quiet)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
$Job=[IO.Path]::GetFullPath($Job)
$result=[ordered]@{status='failed';package=$Package;root=$Root;backup=(Join-Path $Job 'backup');error='';rollback_errors=@()}
$mutex=$null;$locked=$false;$installLock=$null;$applied=New-Object Collections.Generic.List[object]
function Hash([string]$Path){$stream=[IO.File]::OpenRead($Path);$algorithm=[Security.Cryptography.SHA256]::Create();try{return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','')}finally{$stream.Dispose();$algorithm.Dispose()}}
function AssertPlain([string]$Path){
    $current=[IO.Path]::GetFullPath($Path)
    while($current -and $current.Length -ge $Root.Length){
        if(Test-Path -LiteralPath $current){if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "不支持重解析路径：$current"}}
        if($current -eq $Root){break};$current=[IO.Path]::GetDirectoryName($current)
    }
}
function Target([string]$Relative){
    $p=[IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if(!$p.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){throw '更新路径越界。'}
    AssertPlain $p;return $p
}
function Allowed([string]$Name){
    return $Name -match '^(ShellBot\.exe|WinShellBot\.exe|ShellBot-SandboxProbe\.exe|ShellBotUpdate\.exe|pdfium\.dll|README\.md|LICENSE\.txt|manifest\.json|validation-summary\.json|SHA256SUMS\.json)$' -or $Name -match '^(docs|licenses)/[^/].*'
}
try{
    if(!(Test-Path -LiteralPath (Join-Path $Root 'WinShellBot.exe') -PathType Leaf)){throw '找不到核心程序 WinShellBot.exe，请将更新器放在 ShellBot 根目录。'}
    AssertPlain $Root;AssertPlain $Job
    if(!$Job.StartsWith($Root+'\.shellbot-update\job-',[StringComparison]::OrdinalIgnoreCase)){throw '无效更新暂存目录。'}
    $nameBytes=[Text.Encoding]::UTF8.GetBytes($Root.ToLowerInvariant());$hash=[Security.Cryptography.SHA256]::Create();$key=([BitConverter]::ToString($hash.ComputeHash($nameBytes))).Replace('-','');$hash.Dispose()
    $mutex=New-Object Threading.Mutex($false,('Local\ShellBotUpdate-'+$key));try{$locked=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(!$locked){throw '当前目录已有更新正在进行。'}
    $installLock=[IO.File]::Open((Join-Path $Root '.shellbot-update\update.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    if($ParentId){$parent=Get-Process -Id $ParentId -ErrorAction SilentlyContinue;if($parent -and !$parent.WaitForExit(10000)){throw '更新启动程序尚未退出。'}}
    if([IO.Path]::GetExtension($Package) -ine '.zip'){throw '目前仅支持本地 ZIP 安装包。'}
    $zip=[IO.Compression.ZipFile]::OpenRead($Package)
    $payload=Join-Path $Job 'payload';[IO.Directory]::CreateDirectory($payload)|Out-Null
    $entries=New-Object 'Collections.Generic.Dictionary[string,object]' ([StringComparer]::OrdinalIgnoreCase)
    $total=[uint64]0
    try{
        foreach($e in $zip.Entries){
            $n=$e.FullName
            if(!$n -or $n.Contains('\') -or $n.Contains(':') -or $n.StartsWith('/') -or $n -match '(^|/)\.{1,2}(/|$)' -or $n -match '[\x00-\x1f]' -or (($e.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000 -or ($e.ExternalAttributes -band 0x400)){throw 'ZIP 包含不安全路径或链接。'}
            foreach($part in $n.TrimEnd('/').Split('/')){if(!$part -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|$)'){throw 'ZIP 包含无效 Windows 文件名。'}}
            if($n.EndsWith('/')){continue}
            if($entries.ContainsKey($n)){throw 'ZIP 包含重复文件名。'}
            $entries.Add($n,$e);$total+=[uint64]$e.Length
            if($e.Length -gt 4GB -or $total -gt 16GB -or $entries.Count -gt 100000){throw 'ZIP 超出安全展开预算。'}
        }
        $cores=@($entries.Keys|Where-Object{$_ -match '(^|/)WinShellBot\.exe$'})
        if($cores.Count -ne 1){throw 'ZIP 必须包含唯一的 WinShellBot.exe。'}
        $prefix=$cores[0].Substring(0,$cores[0].Length-'WinShellBot.exe'.Length)
        if($prefix.TrimEnd('/').Contains('/')){throw 'ZIP 仅支持平铺或一个顶层安装包文件夹。'}
        $files=New-Object 'Collections.Generic.Dictionary[string,object]' ([StringComparer]::OrdinalIgnoreCase)
        foreach($n in $entries.Keys){
            if(!$n.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'ZIP 存在安装包目录之外的文件。'}
            $relative=$n.Substring($prefix.Length)
            if(!(Allowed $relative)){throw "安装包包含未允许的文件：$relative"}
            $files.Add($relative,$entries[$n])
        }
        foreach($required in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll','SHA256SUMS.json')){if(!$files.ContainsKey($required)){throw "安装包缺少：$required"}}
        foreach($n in $files.Keys){
            $dest=[IO.Path]::GetFullPath((Join-Path $payload $n));if(!$dest.StartsWith($payload+'\',[StringComparison]::OrdinalIgnoreCase)){throw '解压路径越界。'}
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest))|Out-Null
            $entryStream=$files[$n].Open();$output=[IO.File]::Create($dest)
            try{$buffer=New-Object byte[] 1048576;$written=[uint64]0;while(($count=$entryStream.Read($buffer,0,$buffer.Length)) -gt 0){$written+=$count;if($written -gt $files[$n].Length){throw 'ZIP 实际展开量与声明不符。'};$output.Write($buffer,0,$count)};if($written -ne $files[$n].Length){throw 'ZIP 文件不完整。'}}finally{$output.Dispose();$entryStream.Dispose()}
        }
    }finally{$zip.Dispose()}
    $seen=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $manifest=Get-Content -LiteralPath (Join-Path $payload 'SHA256SUMS.json') -Raw -Encoding UTF8|ConvertFrom-Json
    foreach($row in $manifest){
        if(!($row.path -is [string]) -or !$files.ContainsKey($row.path) -or !$seen.Add($row.path) -or $row.sha256 -notmatch '^[0-9a-fA-F]{64}$'){throw '安装包校验清单无效。'}
        if((Hash (Join-Path $payload $row.path)) -ine $row.sha256){throw "文件校验失败：$($row.path)"}
    }
    foreach($n in $files.Keys){if($n -ine 'SHA256SUMS.json' -and !$seen.Contains($n)){throw "文件未列入校验清单：$n"}}
    foreach($n in @('ShellBot.exe','WinShellBot.exe','ShellBot-SandboxProbe.exe','pdfium.dll')){
        $stream=[IO.File]::OpenRead((Join-Path $payload $n));try{if($stream.ReadByte() -ne 77 -or $stream.ReadByte() -ne 90){throw "无效运行文件：$n"}}finally{$stream.Dispose()}
        if([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $payload $n)).ProductName -notmatch 'ShellBot|PDFium'){throw "运行文件产品标识不符：$n"}
    }
    # 仅请求相邻核心正常退出，不按进程名称强杀其他安装。
    $running=@(Get-Process -Name WinShellBot,ShellBot,ShellBot-SandboxProbe -ErrorAction SilentlyContinue|Where-Object{try{[IO.Path]::GetDirectoryName($_.MainModule.FileName) -ieq $Root}catch{$false}})
    if($running.Count){
        $data=Join-Path $Root 'Data';$locator=Join-Path $Root 'ShellBot.storage.json'
        if(Test-Path -LiteralPath $locator){$location=Get-Content -LiteralPath $locator -Raw -Encoding UTF8|ConvertFrom-Json;if($location.format_version -ne 1 -or !$location.directory){throw '数据位置设置无效，请先从托盘退出 ShellBot。'};if($location.directory -ne 'Data'){$data=[IO.Path]::GetFullPath($location.directory)}}
        $manager=Join-Path $data 'Desktop\Manager';if($manager.Contains('"')){throw '无效的数据位置。'}
        $info=New-Object Diagnostics.ProcessStartInfo;$info.FileName=Join-Path $Root 'WinShellBot.exe';$info.Arguments='--manager-stop --manager-data-dir "'+$manager+'"';$info.UseShellExecute=$false;$info.CreateNoWindow=$true
        $stop=[Diagnostics.Process]::Start($info);if(!$stop.WaitForExit(15000)){throw 'ShellBot 未能正常退出，请从托盘退出后重试。'}
    }
    foreach($proc in $running){if(!$proc.WaitForExit(10000)){throw '仍有 ShellBot 程序占用安装目录，请退出后重试。'}}
    [IO.Directory]::CreateDirectory($result.backup)|Out-Null
    $plan=New-Object Collections.Generic.List[object]
    foreach($n in $files.Keys){
        $dest=Target $n;$exists=Test-Path -LiteralPath $dest
        if($exists){$pin=[IO.File]::Open($dest,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$pin.Dispose();$backup=Join-Path $result.backup $n;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($backup))|Out-Null;[IO.File]::Copy($dest,$backup,$false)}
        $plan.Add(@{name=$n;destination=$dest;existed=$exists})
    }
    foreach($item in $plan){
        $dest=Target $item.name;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest))|Out-Null
        $applied.Add($item);[IO.File]::Copy((Join-Path $payload $item.name),$dest,$true)
        if((Hash $dest) -ine (Hash (Join-Path $payload $item.name))){throw "写入后校验失败：$($item.name)"}
    }
    $result.status='completed'
    # payload 是本次新建且已核对范围的缓存；备份、结果及用户 ZIP 保留。
    $clean=[IO.Path]::GetFullPath($payload);if(!$clean.StartsWith($Job+'\',[StringComparison]::OrdinalIgnoreCase)){throw '缓存目录越界。'}
    # 文件版本检查或外部读取可能短暂占用 DLL；只重试本次 payload，不动备份或用户 ZIP。
    for($attempt=1;$attempt -le 9;$attempt++){
        $result.cache_cleanup_attempts=$attempt
        try{if(Test-Path -LiteralPath $clean){Remove-Item -LiteralPath $clean -Recurse -Force};break}
        catch{if($attempt -eq 9){$result.cache_warning=$_.Exception.Message}else{Start-Sleep -Milliseconds 250}}
    }
}catch{
    $result.error=$_.Exception.Message
    for($i=$applied.Count-1;$i -ge 0;$i--){$item=$applied[$i];try{$dest=Target $item.name;if($item.existed){[IO.File]::Copy((Join-Path $result.backup $item.name),$dest,$true)}elseif(Test-Path -LiteralPath $dest){[IO.File]::Delete($dest)}}catch{$result.rollback_errors+= $_.Exception.Message}}
    if($result.rollback_errors.Count){$result.status='recovery_required'}
}finally{
    $result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $Job 'result.json') -Encoding UTF8
    if($installLock){$installLock.Dispose()};if($locked){$mutex.ReleaseMutex()};if($mutex){$mutex.Dispose()}
}
if(!$Quiet){Add-Type -AssemblyName System.Windows.Forms;$message=if($result.status -eq 'completed'){'更新完成，可以重新启动 ShellBot。备份：'+$result.backup}else{'更新失败：'+$result.error+'。记录：'+$Job};[Windows.Forms.MessageBox]::Show($message,'ShellBotUpdate')|Out-Null}
if($result.status -ne 'completed'){exit 1}
