[CmdletBinding()]
param([switch]$Next,[string]$Version,[switch]$Check,[string]$PrintNext)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$utf8=[Text.UTF8Encoding]::new($false)
function Parts([string]$value){
    if($value -cnotmatch '^([1-9][0-9]*)\.([0-9])\.([0-9])$'){throw 'Version must be a positive major followed by two single digits (0-9).'}
    @([System.Numerics.BigInteger]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture),[int]$Matches[2],[int]$Matches[3])
}
function Increment([string]$value){
    $parts=Parts $value
    $parts[2]++
    if($parts[2] -eq 10){$parts[2]=0;$parts[1]++}
    if($parts[1] -eq 10){$parts[1]=0;$parts[0]=[System.Numerics.BigInteger]::Add($parts[0],[System.Numerics.BigInteger]::One)}
    '{0}.{1}.{2}' -f $parts[0].ToString([Globalization.CultureInfo]::InvariantCulture),$parts[1],$parts[2]
}
if($PSBoundParameters.ContainsKey('PrintNext')){
    if($Next -or $Check -or $Version){throw 'PrintNext is a calculation only; do not combine modes.'}
    Increment $PrintNext
    return
}
if(([int][bool]$Next+[int][bool]$Check+[int][bool]$Version) -ne 1){throw 'Choose exactly one: -Next, -Version <value>, or -Check.'}
$headerPath=Join-Path $root 'core/include/sb_version.h'
$cmakePath=Join-Path $root 'CMakeLists.txt'
$readmePath=Join-Path $root 'README.md'
$header=[IO.File]::ReadAllText($headerPath)
if($header -notmatch '(?m)^#define SB_VERSION "([^"]+)"'){throw 'Missing SB_VERSION.'}
$current=$Matches[1]
$target=if($Next){Increment $current}elseif($Version){$Version}else{$current}
$parts=Parts $target
# The full product version is the string. Windows fixed binary fields are WORDs.
$low=[int]($parts[0] % 65536);$high=[int]([System.Numerics.BigInteger]::Divide($parts[0],65536) % 65536)
$resource='{0},{1},{2},{3}' -f $low,$parts[1],$parts[2],$high
$cmake=[IO.File]::ReadAllText($cmakePath);$readme=[IO.File]::ReadAllText($readmePath)
if($Check){
    if($header -notmatch ('(?m)^#define SB_VERSION_RESOURCE '+[regex]::Escape($resource)+'\s*$') -or
       $cmake -notmatch ('project\(ShellBot VERSION '+[regex]::Escape($target)+' LANGUAGES C\)') -or
       $readme -notmatch ('\A# ShellBot '+[regex]::Escape($target)+'\r?\n')){throw 'Version header, resource, CMake and README disagree.'}
    $target
    return
}
$updatedHeader=[regex]::Replace($header,'(?m)^#define SB_VERSION "[^"]+"','#define SB_VERSION "'+$target+'"')
$updatedHeader=[regex]::Replace($updatedHeader,'(?m)^#define SB_VERSION_RESOURCE [^\r\n]+','#define SB_VERSION_RESOURCE '+$resource)
$updatedCmake=[regex]::Replace($cmake,'project\(ShellBot VERSION [^ ]+ LANGUAGES C\)','project(ShellBot VERSION '+$target+' LANGUAGES C)')
$updatedReadme=$readme.Replace($current,$target)
[IO.File]::WriteAllText($headerPath,$updatedHeader,$utf8)
[IO.File]::WriteAllText($cmakePath,$updatedCmake,$utf8)
[IO.File]::WriteAllText($readmePath,$updatedReadme,$utf8)
$target
