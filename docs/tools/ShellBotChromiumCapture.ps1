<#
.SYNOPSIS
Capture the content surface of one explicitly selected local Chromium target.
.DESCRIPTION
Read-only companion for Windows. The owner must start a separate automation
instance with a loopback DevTools endpoint and explicitly supply its target ID.
This script never enables debugging, scans profiles, evaluates page JavaScript,
sends input, changes focus or closes the application. UIA remains a separate tool.
The captured page content does not include browser chrome or other native windows.
#>
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Endpoint,
 [Parameter(Mandatory=$true)][string]$TargetId,
 [Parameter(Mandatory=$true)][string]$OutputPath,
 [long]$MaxDimension=8192,
 [ValidateRange(0,60)][int]$WaitSeconds=0,
 [ValidateRange(1,120)][int]$TimeoutSeconds=15,
 [long]$MaxPngBytes=16777216,
 [uint32]$ExpectedProcessId=0,
 [string]$ExpectedExecutable=''
)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$reply=[ordered]@{ok=$false;tool='ShellBotChromiumCapture';version='1.1.7';stage='arguments';read_only=$true;automatic_replay=$false}
$client=$null;$handler=$null;$socket=$null;$direct=$null;$script:captureRequestId=0;$outputCreated=$false
function Refuse([string]$code,[string]$message){$exception=[InvalidOperationException]::new($message);$exception.Data['code']=$code;throw $exception}
function Refuse-Limit([string]$code,[string]$field,[long]$actual,[long]$maximum,[string]$unit,[long]$minimum=0){
 $exception=[InvalidOperationException]::new(($field+' is '+$actual+' '+$unit+'; allowed range '+$minimum+'..'+$maximum+' '+$unit+'.'))
 $exception.Data['code']=$code;$exception.Data['field']=$field;$exception.Data['actual']=$actual;$exception.Data['maximum']=$maximum;$exception.Data['unit']=$unit;if($minimum){$exception.Data['minimum']=$minimum};throw $exception
}
try{
 if($MaxDimension -lt 200 -or $MaxDimension -gt 8192){Refuse-Limit 'ARGUMENT_OUT_OF_RANGE' 'MaxDimension' $MaxDimension 8192 'pixels' 200}
 if($MaxPngBytes -lt 1048576 -or $MaxPngBytes -gt 134217728){Refuse-Limit 'ARGUMENT_OUT_OF_RANGE' 'MaxPngBytes' $MaxPngBytes 134217728 'bytes' 1048576}
 $endpointUri=$null
 if(![Uri]::TryCreate($Endpoint,[UriKind]::Absolute,[ref]$endpointUri) -or $endpointUri.Scheme -ne 'http' -or $endpointUri.UserInfo -or $endpointUri.Query -or $endpointUri.Fragment -or $endpointUri.AbsolutePath -ne '/' -or $endpointUri.Host -notin @('127.0.0.1','[::1]','::1') -or $endpointUri.Port -lt 1){Refuse 'INVALID_ENDPOINT' 'Endpoint must be an explicit http://127.0.0.1:port or http://[::1]:port root, without credentials, path, query or fragment.'}
 if($TargetId -cnotmatch '^[A-Za-z0-9_-]{1,128}$'){Refuse 'INVALID_TARGET_ID' 'TargetId must be the exact ID of the explicitly selected page target.'}
 $output=[IO.Path]::GetFullPath($OutputPath)
 if($output -notmatch '^[A-Za-z]:\\' -or [IO.Path]::GetExtension($output) -ine '.png'){Refuse 'INVALID_OUTPUT_PATH' 'OutputPath must resolve to a local PNG file path.'}
 if([IO.File]::Exists($output) -or [IO.Directory]::Exists($output)){Refuse 'OUTPUT_EXISTS' 'OutputPath already exists; no file was overwritten.'}
 $parent=[IO.Path]::GetDirectoryName($output);if(![IO.Directory]::Exists($parent)){Refuse 'OUTPUT_DIRECTORY_MISSING' 'The output directory must already exist.'}
 $part=$parent
 while($part){if(([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0){Refuse 'OUTPUT_REPARSE_POINT' 'Output directory cannot pass through a reparse point.'};$next=[IO.Path]::GetDirectoryName($part.TrimEnd('\'));if($next -eq $part){break};$part=$next}
 if(!('ShellBotChromiumOwner117' -as [type])){Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;using System.ComponentModel;using System.Net;using System.Text;using System.Security.Principal;
public sealed class ShellBotChromiumIdentity117 {public uint pid;public uint session_id;public string executable;public string user_sid;public ulong created;public uint integrity_rid;public bool elevated;public bool appcontainer;}
public static class ShellBotChromiumOwner117 {
 [DllImport("iphlpapi.dll",SetLastError=true)]static extern uint GetExtendedTcpTable(IntPtr p,ref int size,bool sort,int family,int table,uint reserved);
 [DllImport("kernel32.dll",SetLastError=true)]static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool ProcessIdToSessionId(uint pid,out uint session);
 [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)]static extern bool QueryFullProcessImageName(IntPtr p,uint flags,StringBuilder path,ref int size);
 [StructLayout(LayoutKind.Sequential)]struct Time {public uint low,high;}
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool GetProcessTimes(IntPtr p,out Time created,out Time exit,out Time kernel,out Time user);
 [DllImport("kernel32.dll",SetLastError=true)]static extern uint WaitForSingleObject(IntPtr h,uint timeout);
 [DllImport("advapi32.dll",SetLastError=true)]static extern bool OpenProcessToken(IntPtr p,uint access,out IntPtr token);
 [DllImport("advapi32.dll",SetLastError=true)]static extern bool GetTokenInformation(IntPtr token,int kind,IntPtr data,int bytes,out int needed);
 [DllImport("advapi32.dll")]static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
 [DllImport("advapi32.dll")]static extern IntPtr GetSidSubAuthority(IntPtr sid,uint sub);
 static IntPtr TokenData(IntPtr token,int kind){int bytes=0;GetTokenInformation(token,kind,IntPtr.Zero,0,out bytes);if(bytes<1 || bytes>65536)throw new InvalidOperationException("Invalid token information size");IntPtr data=Marshal.AllocHGlobal(bytes);if(!GetTokenInformation(token,kind,data,bytes,out bytes)){int error=Marshal.GetLastWin32Error();Marshal.FreeHGlobal(data);throw new Win32Exception(error,"GetTokenInformation failed");}return data;}
 static uint Listener(string host,int port){IPAddress ip=IPAddress.Parse(host.Trim('[',']'));int family=ip.AddressFamily==System.Net.Sockets.AddressFamily.InterNetwork?2:23;int rowSize=family==2?24:56;uint owner=0;int matches=0;
  for(int attempt=0;attempt<3;attempt++){int bytes=0;uint result=GetExtendedTcpTable(IntPtr.Zero,ref bytes,false,family,5,0);if(result!=122 || bytes<4 || bytes>67108864)throw new Win32Exception((int)result,"Cannot size TCP owner table");IntPtr table=Marshal.AllocHGlobal(bytes);try{result=GetExtendedTcpTable(table,ref bytes,false,family,5,0);if(result==122)continue;if(result!=0)throw new Win32Exception((int)result,"Cannot read TCP owner table");int count=Marshal.ReadInt32(table);if(count<0 || 4L+(long)count*rowSize>bytes)throw new InvalidOperationException("Invalid TCP owner table");for(int n=0;n<count;n++){IntPtr row=IntPtr.Add(table,4+n*rowSize);int state=Marshal.ReadInt32(row,family==2?0:48);int rawPort=Marshal.ReadInt32(row,family==2?8:20);int localPort=((rawPort>>8)&255)|((rawPort&255)<<8);if(state!=2 || localPort!=port)continue;byte[] address=new byte[family==2?4:16];Marshal.Copy(IntPtr.Add(row,family==2?4:0),address,0,address.Length);IPAddress local=new IPAddress(address);if(!local.Equals(ip))continue;owner=(uint)Marshal.ReadInt32(row,family==2?20:52);matches++;}if(matches!=1 || owner==0)throw new InvalidOperationException("Endpoint must have exactly one listener bound to the specified loopback address");return owner;}finally{Marshal.FreeHGlobal(table);}}
  throw new InvalidOperationException("TCP owner table changed repeatedly");
 }
 public static ShellBotChromiumIdentity117 Read(string host,int port){uint pid=Listener(host,port);IntPtr process=OpenProcess(0x101000,false,pid),token=IntPtr.Zero;if(process==IntPtr.Zero)throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot query endpoint owner");try{if(WaitForSingleObject(process,0)!=258)throw new InvalidOperationException("Endpoint owner exited");ShellBotChromiumIdentity117 value=new ShellBotChromiumIdentity117();value.pid=pid;if(!ProcessIdToSessionId(pid,out value.session_id))throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot query owner session");StringBuilder path=new StringBuilder(32768);int length=path.Capacity;if(!QueryFullProcessImageName(process,0,path,ref length))throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot query owner executable");value.executable=path.ToString();Time created,exit,kernel,user;if(!GetProcessTimes(process,out created,out exit,out kernel,out user))throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot query owner creation time");value.created=((ulong)created.high<<32)|created.low;if(!OpenProcessToken(process,8,out token))throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot query owner token");IntPtr data=TokenData(token,1);try{value.user_sid=new SecurityIdentifier(Marshal.ReadIntPtr(data)).Value;}finally{Marshal.FreeHGlobal(data);}data=TokenData(token,25);try{IntPtr sid=Marshal.ReadIntPtr(data);byte count=Marshal.ReadByte(GetSidSubAuthorityCount(sid));if(count==0)throw new InvalidOperationException("Invalid integrity SID");value.integrity_rid=(uint)Marshal.ReadInt32(GetSidSubAuthority(sid,(uint)count-1));}finally{Marshal.FreeHGlobal(data);}data=TokenData(token,20);try{value.elevated=Marshal.ReadInt32(data)!=0;}finally{Marshal.FreeHGlobal(data);}data=TokenData(token,29);try{value.appcontainer=Marshal.ReadInt32(data)!=0;}finally{Marshal.FreeHGlobal(data);}return value;}finally{if(token!=IntPtr.Zero)CloseHandle(token);CloseHandle(process);}}
}
'@}
 if(!('ShellBotChromiumSocket117' -as [type])){Add-Type -TypeDefinition @'
using System;using System.Net;using System.Net.Sockets;using System.IO;using System.Text;using System.Security.Cryptography;using System.Collections.Generic;using System.Threading;
public sealed class ShellBotChromiumSocket117:IDisposable {
 public TcpClient client;public NetworkStream stream;
 static Exception Error(string code,string message){InvalidOperationException error=new InvalidOperationException(message);error.Data["code"]=code;return error;}
 public static ShellBotChromiumSocket117 Open(string host,int port,string path,int timeoutMs){ShellBotChromiumSocket117 connection=new ShellBotChromiumSocket117();try{
  IPAddress ip=IPAddress.Parse(host);if(!IPAddress.IsLoopback(ip))throw Error("INVALID_ENDPOINT","WebSocket address is not loopback");connection.client=new TcpClient(ip.AddressFamily);var connect=connection.client.ConnectAsync(ip,port);if(!connect.Wait(timeoutMs))throw Error("CONNECT_TIMEOUT","Loopback WebSocket connection timed out");connect.GetAwaiter().GetResult();connection.stream=connection.client.GetStream();connection.stream.ReadTimeout=timeoutMs;connection.stream.WriteTimeout=timeoutMs;
  byte[] nonce=new byte[16];using(RandomNumberGenerator random=RandomNumberGenerator.Create()){random.GetBytes(nonce);}string key=Convert.ToBase64String(nonce);string authority=(ip.AddressFamily==AddressFamily.InterNetworkV6?"["+host+"]":host)+":"+port;
  string request="GET "+path+" HTTP/1.1\r\nHost: "+authority+"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "+key+"\r\n\r\n";byte[] bytes=Encoding.ASCII.GetBytes(request);connection.stream.Write(bytes,0,bytes.Length);
  MemoryStream memory=new MemoryStream();int matched=0;DateTime limit=DateTime.UtcNow.AddMilliseconds(timeoutMs);byte[] end=new byte[]{13,10,13,10};while(matched<4){if(DateTime.UtcNow>=limit)throw Error("CONNECT_TIMEOUT","Loopback WebSocket handshake timed out");connection.stream.ReadTimeout=Math.Max(1,(int)(limit-DateTime.UtcNow).TotalMilliseconds);int value=connection.stream.ReadByte();if(value<0)throw Error("WEBSOCKET_HANDSHAKE_FAILED","Endpoint closed during WebSocket handshake");memory.WriteByte((byte)value);if(memory.Length>65536)throw Error("HTTP_HEADER_LIMIT","WebSocket handshake headers exceed 64 KiB");matched=value==end[matched]?matched+1:value==13?1:0;}
  string[] lines=Encoding.ASCII.GetString(memory.ToArray()).Split(new string[]{"\r\n"},StringSplitOptions.None);memory.Dispose();string[] status=lines[0].Split(' ');int code=0;if(status.Length<2 || !Int32.TryParse(status[1],out code))throw Error("WEBSOCKET_HANDSHAKE_FAILED","Invalid HTTP handshake status");if(code>=300 && code<400)throw Error("HTTP_REDIRECT_REFUSED","WebSocket endpoint returned HTTP "+code+"; redirects are not followed");if(code!=101)throw Error("WEBSOCKET_HANDSHAKE_FAILED","WebSocket endpoint returned HTTP "+code);
  Dictionary<string,string> headers=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);for(int i=1;i<lines.Length;i++){if(lines[i].Length==0)continue;int colon=lines[i].IndexOf(':');if(colon<1)throw Error("WEBSOCKET_HANDSHAKE_FAILED","Malformed WebSocket handshake header");string name=lines[i].Substring(0,colon).Trim(),value=lines[i].Substring(colon+1).Trim();if(headers.ContainsKey(name))throw Error("WEBSOCKET_HANDSHAKE_FAILED","Duplicate WebSocket handshake header");headers[name]=value;}
  string upgrade,accept,tokens;if(!headers.TryGetValue("Upgrade",out upgrade) || !String.Equals(upgrade,"websocket",StringComparison.OrdinalIgnoreCase) || !headers.TryGetValue("Connection",out tokens))throw Error("WEBSOCKET_HANDSHAKE_FAILED","Missing WebSocket upgrade headers");bool connectionUpgrade=false;foreach(string token in tokens.Split(',')){if(String.Equals(token.Trim(),"upgrade",StringComparison.OrdinalIgnoreCase))connectionUpgrade=true;}if(!connectionUpgrade || !headers.TryGetValue("Sec-WebSocket-Accept",out accept))throw Error("WEBSOCKET_HANDSHAKE_FAILED","Missing WebSocket accept header");string expected;using(SHA1 sha=SHA1.Create()){expected=Convert.ToBase64String(sha.ComputeHash(Encoding.ASCII.GetBytes(key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));}if(!String.Equals(expected,accept,StringComparison.Ordinal))throw Error("WEBSOCKET_HANDSHAKE_FAILED","WebSocket accept digest mismatch");connection.stream.ReadTimeout=timeoutMs;return connection;
 }catch{connection.Dispose();throw;}}
 public static CancellationTokenRegistration AbortOnCancel(CancellationToken token,IDisposable response){return token.Register(delegate{try{response.Dispose();}catch{}});}
 public void Dispose(){if(stream!=null){stream.Dispose();stream=null;}if(client!=null){client.Close();client=null;}}
}
'@}
 $reply.stage='endpoint_identity';$reply.endpoint=$endpointUri.AbsoluteUri;$reply.target_id=$TargetId
 $hostAddress=$endpointUri.Host.Trim('[',']');$owner=[ShellBotChromiumOwner117]::Read($hostAddress,$endpointUri.Port)
 $caller=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value;$callerSession=[Diagnostics.Process]::GetCurrentProcess().SessionId
 if($owner.user_sid -ne $caller -or $owner.session_id -ne $callerSession -or $owner.integrity_rid -lt 8192 -or $owner.integrity_rid -ge 12288 -or $owner.elevated -or $owner.appcontainer){Refuse 'ENDPOINT_OWNER_REFUSED' 'Endpoint owner must be a non-elevated medium-integrity process in the same user account and Windows session.'}
 if($ExpectedProcessId -and $owner.pid -ne $ExpectedProcessId){Refuse 'ENDPOINT_PID_MISMATCH' 'Endpoint listener PID differs from ExpectedProcessId.'}
 if($ExpectedExecutable -and ![string]::Equals($owner.executable,[IO.Path]::GetFullPath($ExpectedExecutable),[StringComparison]::OrdinalIgnoreCase)){Refuse 'ENDPOINT_EXECUTABLE_MISMATCH' 'Endpoint executable differs from ExpectedExecutable.'}
 $reply.browser_process=[ordered]@{pid=$owner.pid;session_id=$owner.session_id;executable=$owner.executable;creation_time_utc=[DateTime]::FromFileTimeUtc([long]$owner.created).ToString('o');integrity_rid=$owner.integrity_rid;elevated=$owner.elevated;same_user=$true}
 function Verify-Owner{
  $now=[ShellBotChromiumOwner117]::Read($hostAddress,$endpointUri.Port)
  if($now.pid -ne $owner.pid -or $now.created -ne $owner.created -or $now.session_id -ne $owner.session_id -or $now.user_sid -ne $owner.user_sid -or $now.integrity_rid -ne $owner.integrity_rid -or $now.elevated -or $now.appcontainer -or ![string]::Equals($now.executable,$owner.executable,[StringComparison]::OrdinalIgnoreCase)){Refuse 'ENDPOINT_IDENTITY_CHANGED' 'Endpoint owner changed during capture; no image was saved.'}
 }
 Add-Type -AssemblyName System.Net.Http
 $handler=[Net.Http.HttpClientHandler]::new();$handler.AllowAutoRedirect=$false;$handler.UseProxy=$false;$client=[Net.Http.HttpClient]::new($handler);$client.Timeout=[TimeSpan]::FromSeconds($TimeoutSeconds);$client.MaxResponseContentBufferSize=1048576
 function Read-EndpointJson([string]$path){
  $response=$null;$registration=$null;$cancel=[Threading.CancellationTokenSource]::new($TimeoutSeconds*1000)
  try{
   $response=$client.GetAsync([Uri]::new($endpointUri,$path),[Net.Http.HttpCompletionOption]::ResponseHeadersRead,$cancel.Token).GetAwaiter().GetResult()
   # A native .NET callback aborts this request on timeout; no PowerShell runs on its timer thread.
   $registration=[ShellBotChromiumSocket117]::AbortOnCancel($cancel.Token,$response)
   $status=[int]$response.StatusCode;if($status -ge 300 -and $status -lt 400){Refuse 'HTTP_REDIRECT_REFUSED' ('Endpoint returned HTTP '+$status+'; redirects are not followed.')}
   if(!$response.IsSuccessStatusCode){Refuse 'HTTP_ENDPOINT_ERROR' ('Endpoint returned HTTP '+$status+'.')}
   if($response.Content.Headers.ContentLength -gt 1048576){Refuse-Limit 'ENDPOINT_METADATA_LIMIT' 'endpoint.metadata_bytes' $response.Content.Headers.ContentLength 1048576 'bytes'}
   $stream=$response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();$memory=[IO.MemoryStream]::new();$buffer=New-Object byte[] 16384
   try{while(($got=$stream.ReadAsync($buffer,0,$buffer.Length,$cancel.Token).GetAwaiter().GetResult()) -gt 0){if($memory.Length+$got -gt 1048576){Refuse-Limit 'ENDPOINT_METADATA_LIMIT' 'endpoint.metadata_bytes' ($memory.Length+$got) 1048576 'bytes'};$memory.Write($buffer,0,$got)};$text=[Text.Encoding]::UTF8.GetString($memory.ToArray());return ($text|ConvertFrom-Json)}finally{$stream.Dispose();$memory.Dispose()}
  }finally{if($registration){$registration.Dispose()};if($response){$response.Dispose()};$cancel.Dispose()}
 }
 $reply.stage='target_metadata';$targets=Read-EndpointJson 'json/list';$target=@($targets|Where-Object {$_.id -ceq $TargetId})
 if($target.Count -ne 1 -or $target[0].type -notin @('page','webview')){Refuse 'TARGET_NOT_FOUND' 'The explicitly selected page target was not found at this endpoint.'};$target=$target[0]
 $socketUri=[Uri]$target.webSocketDebuggerUrl;$expectedPath='/devtools/page/'+$TargetId
 if($socketUri.Scheme -ne 'ws' -or $socketUri.UserInfo -or $socketUri.Query -or $socketUri.Fragment -or $socketUri.Host.Trim('[',']') -cne $hostAddress -or $socketUri.Port -ne $endpointUri.Port -or $socketUri.AbsolutePath -cne $expectedPath){Refuse 'TARGET_ENDPOINT_MISMATCH' 'Selected target returned a different or non-loopback WebSocket endpoint.'}
 Verify-Owner
 $reply.target=[ordered]@{id=$TargetId;type=$target.type;title=$target.title;url=$target.url}
 $reply.stage='connect';$direct=[ShellBotChromiumSocket117]::Open($hostAddress,$endpointUri.Port,$socketUri.AbsolutePath,$TimeoutSeconds*1000)
 if([Net.WebSockets.WebSocket].GetMethods().Name -contains 'CreateFromStream'){$socket=[Net.WebSockets.WebSocket]::CreateFromStream($direct.stream,$false,[NullString]::Value,[TimeSpan]::FromSeconds(20))}
 else{$socket=[Net.WebSockets.WebSocket]::CreateClientWebSocket($direct.stream,[NullString]::Value,65536,65536,[TimeSpan]::FromSeconds(20),$false,[Net.WebSockets.WebSocket]::CreateClientBuffer(65536,65536))}
 $messageLimit=[long][Math]::Ceiling($MaxPngBytes/3.0)*4+65536
 function Read-Cdp([string]$method,[hashtable]$parameters){
  if($method -notin @('Browser.getVersion','Target.getTargetInfo','Page.enable','Page.getFrameTree','Page.captureScreenshot')){Refuse 'METHOD_REFUSED' 'Only fixed read-only capture methods are available.'}
  $script:captureRequestId++;$requestId=$script:captureRequestId;$cancel=[Threading.CancellationTokenSource]::new($TimeoutSeconds*1000)
  try{
   $bytes=[Text.Encoding]::UTF8.GetBytes((@{id=$requestId;method=$method;params=$parameters}|ConvertTo-Json -Depth 12 -Compress));[void]$socket.SendAsync([ArraySegment[byte]]::new($bytes),[Net.WebSockets.WebSocketMessageType]::Text,$true,$cancel.Token).GetAwaiter().GetResult()
   while($true){$memory=[IO.MemoryStream]::new();try{do{$buffer=New-Object byte[] 65536;$chunk=$socket.ReceiveAsync([ArraySegment[byte]]::new($buffer),$cancel.Token).GetAwaiter().GetResult();if($chunk.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text){Refuse 'CDP_CONNECTION_CLOSED' 'CDP returned a non-text message or closed before the reply.'};if($memory.Length+$chunk.Count -gt $messageLimit){Refuse-Limit 'CDP_RESPONSE_LIMIT' 'cdp.response_bytes' ($memory.Length+$chunk.Count) $messageLimit 'bytes'};$memory.Write($buffer,0,$chunk.Count)}while(!$chunk.EndOfMessage);$message=[Text.Encoding]::UTF8.GetString($memory.ToArray())|ConvertFrom-Json}finally{$memory.Dispose()}
    if($message.id -eq $requestId){if($message.error){$reply.cdp_error=$message.error;$reply.cdp_method=$method;Refuse 'CDP_API_ERROR' ('CDP '+$method+' failed: '+($message.error|ConvertTo-Json -Depth 12 -Compress))};return ,$message.result}
   }
  }finally{$cancel.Dispose()}
 }
 $reply.stage='capture';$version=Read-Cdp 'Browser.getVersion' @{};$reply.browser_version=$version
 $info=Read-Cdp 'Target.getTargetInfo' @{targetId=$TargetId};if($info.targetInfo.targetId -cne $TargetId -or $info.targetInfo.type -notin @('page','webview')){Refuse 'TARGET_IDENTITY_MISMATCH' 'CDP target identity differs from the selected target.'}
 [void](Read-Cdp 'Page.enable' @{});$tree=Read-Cdp 'Page.getFrameTree' @{};$reply.frame_url=$tree.frameTree.frame.url
 if($WaitSeconds){Start-Sleep -Seconds $WaitSeconds;Verify-Owner}
 $capture=Read-Cdp 'Page.captureScreenshot' @{format='png';fromSurface=$true;captureBeyondViewport=$false}
 if(!$capture.data){Refuse 'INVALID_PNG' 'CDP returned empty image data.'}
 if($capture.data.Length -gt [Math]::Ceiling($MaxPngBytes/3.0)*4){Refuse-Limit 'PNG_BYTE_LIMIT' 'capture.data_base64' $capture.data.Length ([long][Math]::Ceiling($MaxPngBytes/3.0)*4) 'Base64 characters'}
 try{$png=[Convert]::FromBase64String($capture.data)}catch{Refuse 'INVALID_IMAGE_BASE64' 'CDP returned invalid Base64 image data.'}
 if($png.Length -gt $MaxPngBytes){Refuse-Limit 'PNG_BYTE_LIMIT' 'png.encoded_bytes' $png.Length $MaxPngBytes 'bytes'}
 if($png.Length -lt 33){Refuse 'INVALID_PNG' 'Decoded PNG is shorter than the required PNG/IHDR header.'}
 $signature=@(137,80,78,71,13,10,26,10);for($i=0;$i -lt 8;$i++){if($png[$i] -ne $signature[$i]){Refuse 'INVALID_PNG' 'CDP image is not a PNG.'}}
 if([Text.Encoding]::ASCII.GetString($png,12,4) -ne 'IHDR'){Refuse 'INVALID_PNG' 'PNG has no leading IHDR.'}
 $width=[uint64]$png[16]*16777216+[uint64]$png[17]*65536+[uint64]$png[18]*256+$png[19];$height=[uint64]$png[20]*16777216+[uint64]$png[21]*65536+[uint64]$png[22]*256+$png[23]
 if(!$width -or !$height){Refuse 'INVALID_PNG' 'PNG dimensions must be positive.'}
 if($width -gt 32768){Refuse-Limit 'PNG_PIXEL_LIMIT' 'png.width' $width 32768 'pixels'}
 if($height -gt 32768){Refuse-Limit 'PNG_PIXEL_LIMIT' 'png.height' $height 32768 'pixels'}
 if($width*$height -gt 67108864){Refuse-Limit 'PNG_PIXEL_LIMIT' 'png.pixels' ($width*$height) 67108864 'pixels'}
 $reply.source_png=[ordered]@{width=$width;height=$height;bytes=$png.Length};$reply.stage='decode_png';Add-Type -AssemblyName System.Drawing
 $sourceStream=[IO.MemoryStream]::new($png,$false);$image=$null;$scaled=$null;$graphics=$null;$encoded=$null
 try{
  $image=[Drawing.Image]::FromStream($sourceStream,$false,$true)
  if($image.Width -ne $width -or $image.Height -ne $height -or $image.RawFormat.Guid -ne [Drawing.Imaging.ImageFormat]::Png.Guid){Refuse 'INVALID_PNG' 'PNG could not be decoded with matching dimensions.'}
  # Clone forces pixel decoding rather than accepting only a readable header.
  $decoded=[Drawing.Bitmap]::new($image)
  try{
   $scale=[Math]::Min(1.0,$MaxDimension/[double][Math]::Max($width,$height));$newWidth=[Math]::Max(1,[int][Math]::Floor($width*$scale));$newHeight=[Math]::Max(1,[int][Math]::Floor($height*$scale))
   if($scale -lt 1){$scaled=[Drawing.Bitmap]::new($newWidth,$newHeight,[Drawing.Imaging.PixelFormat]::Format32bppArgb);$graphics=[Drawing.Graphics]::FromImage($scaled);$graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic;$graphics.DrawImage($decoded,0,0,$newWidth,$newHeight);$encoded=[IO.MemoryStream]::new();$scaled.Save($encoded,[Drawing.Imaging.ImageFormat]::Png);$png=$encoded.ToArray()}
  }finally{$decoded.Dispose()}
 }finally{if($graphics){$graphics.Dispose()};if($scaled){$scaled.Dispose()};if($encoded){$encoded.Dispose()};if($image){$image.Dispose()};$sourceStream.Dispose()}
 if($png.Length -gt $MaxPngBytes){Refuse-Limit 'PNG_BYTE_LIMIT' 'png.encoded_bytes' $png.Length $MaxPngBytes 'bytes'}
 Verify-Owner;$reply.stage='save_png'
 $file=[IO.FileStream]::new($output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None);$outputCreated=$true
 try{$file.Write($png,0,$png.Length);$file.Flush($true)}finally{$file.Dispose()}
 $sha=[Security.Cryptography.SHA256]::Create();try{$hash=([BitConverter]::ToString($sha.ComputeHash($png))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
 $reply.ok=$true;$reply.stage='complete';$reply.output=[ordered]@{path=$output;width=$newWidth;height=$newHeight;bytes=$png.Length;sha256=$hash}
 $reply.capture=[ordered]@{backend='chromium_cdp';from_surface=$true;scope='selected page content only';application_paint_verified=$false;freshness='Verify application state or a known visual change separately; API success alone does not prove new application content.'}
}catch{
 $errorException=$_.Exception;$reply.error=[ordered]@{code=if($errorException.Data['code']){$errorException.Data['code']}else{'CAPTURE_FAILED'};message=$errorException.Message}
 foreach($key in @('field','actual','maximum','unit','minimum')){if($errorException.Data.Contains($key)){$reply.error[$key]=$errorException.Data[$key]}}
 $baseException=$errorException.GetBaseException();if($baseException.Data['code']){$reply.error.code=$baseException.Data['code']};if($baseException -is [ComponentModel.Win32Exception]){$reply.error.win32_error=$baseException.NativeErrorCode};if($baseException.Message -ne $errorException.Message){$reply.error.cause=$baseException.Message}
 if($outputCreated){$reply.partial_output_path=$output;$reply.error.message+=' The newly created output was retained for review.'}
}finally{if($socket){$socket.Dispose()};if($direct){$direct.Dispose()};if($client){$client.Dispose()};if($handler){$handler.Dispose()}}
$reply|ConvertTo-Json -Depth 18 -Compress
if(!$reply.ok){exit 1}
