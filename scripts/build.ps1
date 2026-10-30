param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')][string]$Platform = 'x64',
    [switch]$WithTests
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ($WithTests) {
    if ($Platform -ne 'x64') { throw 'Local test projects are available only for x64.' }
    foreach ($testProject in @('tests\TaskFixture.vcxproj', 'tests\ProjectPolicyTests.vcxproj')) {
        if (!(Test-Path -LiteralPath (Join-Path $repoRoot $testProject))) {
            throw "Local test sources are not included in Git. Restore your local test copy before using -WithTests: $testProject"
        }
    }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere) was not found.' }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Install Visual Studio C++ desktop tools and a recent Windows SDK.' }
& $msbuild (Join-Path $repoRoot 'WinShellBot\WinShellBot.vcxproj') /m /nologo /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform"
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
& $msbuild (Join-Path $repoRoot 'ShellBotUpdate\ShellBotUpdate.vcxproj') /m /nologo /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform"
if ($LASTEXITCODE -ne 0) { throw 'Updater build failed.' }
if ($WithTests) {
    & $msbuild (Join-Path $repoRoot 'tests\TaskFixture.vcxproj') /m /nologo /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform"
    if ($LASTEXITCODE -ne 0) { throw 'Native test fixture build failed.' }
    & $msbuild (Join-Path $repoRoot 'tests\ProjectPolicyTests.vcxproj') /m /nologo /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform"
    if ($LASTEXITCODE -ne 0) { throw 'Project policy test build failed.' }
}
Write-Output (Join-Path $repoRoot "out\$Platform\$Configuration\WinShellBot.exe")
