# Configures and builds Beaglex with MSVC + Ninja.
# Usage: .\build.ps1 [-Config Release|Debug] [-NoTests] [-BuildDir <path>]
# MSVC fails on very long paths; pass a short -BuildDir if the repo lives deep in the filesystem.
param(
    [ValidateSet('Release', 'Debug')] [string] $Config = 'Release',
    [switch] $NoTests,
    [string] $BuildDir
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the C++ toolset was found.' }

$tests = if ($NoTests) { 'OFF' } else { 'ON' }
$buildDir = if ($BuildDir) { $BuildDir } else { "build/$($Config.ToLower())" }

$cmd = "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -no_logo && " +
       "cmake -S . -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=$Config -DBGX_BUILD_TESTS=$tests && " +
       "cmake --build $buildDir"
# VsDevCmd shells out to vswhere, which is not on PATH by default.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$ErrorActionPreference = 'Continue' # native stderr output is not a failure
cmd /c "$cmd 2>&1"
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
