param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')][string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere) was not found.' }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Install Visual Studio C++ desktop tools and a recent Windows SDK.' }
& $msbuild (Join-Path $repoRoot 'WinShellBot\WinShellBot.vcxproj') /m /nologo /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform"
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
if ($Platform -eq 'x64') {
    & $msbuild (Join-Path $repoRoot 'tests\TaskFixture.vcxproj') /m /nologo /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform"
    if ($LASTEXITCODE -ne 0) { throw 'Native test fixture build failed.' }
    & $msbuild (Join-Path $repoRoot 'tests\ProjectPolicyTests.vcxproj') /m /nologo /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform"
    if ($LASTEXITCODE -ne 0) { throw 'Project policy test build failed.' }
}
Write-Output (Join-Path $repoRoot "out\$Platform\$Configuration\WinShellBot.exe")
