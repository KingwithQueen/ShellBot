[CmdletBinding(SupportsShouldProcess=$true,ConfirmImpact='Medium')]
param(
 [ValidateSet('Status','Apply','Restore')][string]$Action='Status',
 [ValidateSet('CurrentUser','AllUsers')][string]$Scope='CurrentUser',
 [string]$BackupPath
)
$ErrorActionPreference='Stop'
# 此工具在发起 mstsc 连接的电脑上运行；默认只读，不关闭连接、不重启、不自动提升权限。
$SubKey='Software\Microsoft\Terminal Server Client'
$ValueName='RemoteDesktop_SuppressWhenMinimized'
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$views=if([Environment]::Is64BitOperatingSystem){@('Registry64','Registry32')}else{@('Registry32')}
function Base([string]$scopeName,[string]$view){
 $hive=if($scopeName -eq 'CurrentUser'){[Microsoft.Win32.RegistryHive]::CurrentUser}else{[Microsoft.Win32.RegistryHive]::LocalMachine}
 return [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,[Microsoft.Win32.RegistryView]::$view)
}
function Snapshot([string]$scopeName,[string]$view){
 $base=Base $scopeName $view;$key=$null
 try{
  $key=$base.OpenSubKey($SubKey,$false);$exists=$key -and ($key.GetValueNames() -contains $ValueName)
  $row=[ordered]@{view=$view;exists=[bool]$exists;kind=$null;data=$null}
  if($exists){
   $kind=$key.GetValueKind($ValueName).ToString();$raw=$key.GetValue($ValueName,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames);$row.kind=$kind
   $row.data=switch($kind){'Binary'{[Convert]::ToBase64String($raw)}'None'{[Convert]::ToBase64String($raw)}'QWord'{$raw.ToString([Globalization.CultureInfo]::InvariantCulture)}'DWord'{[int]$raw}'MultiString'{,@($raw)}default{[string]$raw}}
  };return [PSCustomObject]$row
 }finally{if($key){$key.Dispose()};$base.Dispose()}
}
function EqualValue($left,$right){return (($left|ConvertTo-Json -Compress -Depth 5) -ceq ($right|ConvertTo-Json -Compress -Depth 5))}
function RestoreValue([string]$scopeName,$row){
 $base=Base $scopeName $row.view;$key=$null
 try{
  if(!$row.exists){$key=$base.OpenSubKey($SubKey,$true);if($key){$key.DeleteValue($ValueName,$false)};return}
  $kind=[Microsoft.Win32.RegistryValueKind][Enum]::Parse([Microsoft.Win32.RegistryValueKind],$row.kind)
  $raw=switch($row.kind){'Binary'{,[Convert]::FromBase64String($row.data)}'None'{,[Convert]::FromBase64String($row.data)}'QWord'{[long]::Parse($row.data,[Globalization.CultureInfo]::InvariantCulture)}'DWord'{[int]$row.data}'MultiString'{,[string[]]@($row.data)}default{[string]$row.data}}
  $key=$base.CreateSubKey($SubKey);$key.SetValue($ValueName,$raw,$kind);$key.Flush()
 }finally{if($key){$key.Dispose()};$base.Dispose()}
}
function SaveBackup([string]$path,$value){
 $directory=[IO.Path]::GetDirectoryName($path);[IO.Directory]::CreateDirectory($directory)|Out-Null
 $stream=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 try{$data=[Text.Encoding]::UTF8.GetBytes(($value|ConvertTo-Json -Depth 8));$stream.Write($data,0,$data.Length);$stream.Flush($true)}finally{$stream.Dispose()}
}
try{
 if($Action -eq 'Restore'){
  if(!$BackupPath){throw 'Restore requires -BackupPath from an earlier Apply.'}
  $saved=Get-Content -LiteralPath $BackupPath -Raw -Encoding UTF8|ConvertFrom-Json
  if($saved.format_version -ne 1 -or $saved.computer -cne [Environment]::MachineName -or $saved.user_sid -cne $sid -or $saved.sub_key -cne $SubKey -or $saved.value_name -cne $ValueName -or $saved.scope -notin @('CurrentUser','AllUsers') -or @($saved.entries).Count -ne $views.Count){throw 'Backup does not match this computer, user or fixed setting.'}
  $Scope=$saved.scope;$seen=@{}
  foreach($row in $saved.entries){
   if($row.view -notin $views -or $seen.ContainsKey($row.view) -or !($row.exists -is [bool]) -or ($row.exists -and $row.kind -notin @('String','ExpandString','MultiString','DWord','QWord','Binary','None'))){throw 'Invalid backup entry.'};$seen[$row.view]=$true
   $current=Snapshot $Scope $row.view
   if(!(EqualValue $current $row) -and !($current.exists -and $current.kind -eq 'DWord' -and $current.data -eq 2)){throw 'Setting changed after Apply; refusing to overwrite the newer value.'}
  }
  if($PSCmdlet.ShouldProcess("$Scope $SubKey\$ValueName",'Restore only the saved value; keep unrelated settings')){
   foreach($row in $saved.entries){RestoreValue $Scope $row}
   @{status='restored';scope=$Scope;backup=$BackupPath;entries=@($views|ForEach-Object{Snapshot $Scope $_});reconnect_required=$true}|ConvertTo-Json -Depth 8
  };exit 0
 }
 $entries=@($views|ForEach-Object{Snapshot $Scope $_})
 if($Action -eq 'Status'){
  $machineEntries=@($views|ForEach-Object{Snapshot 'AllUsers' $_})
  @{status='read_only';run_on='RDP client computer; classic mstsc';scope=$Scope;entries=$entries;all_users_entries=$machineEntries;user_setting_enabled=(@($entries|Where-Object{!$_.exists -or $_.kind -ne 'DWord' -or $_.data -ne 2}).Count -eq 0);connections_closed=$false}|ConvertTo-Json -Depth 8;exit 0
 }
 if(!$BackupPath){$BackupPath=Join-Path $env:LOCALAPPDATA ('ShellBot\RdpClient\backup-'+[Guid]::NewGuid().ToString('N')+'.json')};$BackupPath=[IO.Path]::GetFullPath($BackupPath)
 if(!$PSCmdlet.ShouldProcess("$Scope $SubKey\$ValueName",'Back up current values, then set DWORD 2 in both registry views')){exit 0}
 $backup=[ordered]@{format_version=1;computer=[Environment]::MachineName;user_sid=$sid;scope=$Scope;sub_key=$SubKey;value_name=$ValueName;entries=$entries}
 SaveBackup $BackupPath $backup
 $applied=New-Object Collections.Generic.List[object]
 try{
  foreach($row in $entries){$base=Base $Scope $row.view;$key=$null;try{$key=$base.CreateSubKey($SubKey);$applied.Add($row);$key.SetValue($ValueName,2,[Microsoft.Win32.RegistryValueKind]::DWord);$key.Flush()}finally{if($key){$key.Dispose()};$base.Dispose()}}
 }catch{
  $original=$_.Exception.Message;$rollback=@();for($i=$applied.Count-1;$i -ge 0;$i--){try{RestoreValue $Scope $applied[$i]}catch{$rollback+=$_.Exception.Message}}
  @{status='apply_failed';error=$original;rollback_errors=$rollback;backup=$BackupPath}|ConvertTo-Json -Depth 5;exit 1
 }
 @{status='applied';scope=$Scope;backup=$BackupPath;entries=@($views|ForEach-Object{Snapshot $Scope $_});reconnect_required=$true;connections_closed=$false;instruction='Reconnect classic Remote Desktop manually when convenient. This does not unlock a remote session or override ShellBot pause.'}|ConvertTo-Json -Depth 8
}catch{@{status='failed';error=$_.Exception.Message;scope=$Scope}|ConvertTo-Json -Depth 4;exit 1}
