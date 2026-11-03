# Builds and tests on Windows with Visual Studio 2022 (Build Tools are enough), CMake and Ninja.
# Output: <Out>\modavd_extras.dll and <Out>\external_renderer_ipc.dll.
param(
    [string]$Out = (Join-Path $PSScriptRoot 'out'),
    [string]$CMake = 'cmake',
    [string]$Ninja = 'ninja',
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$NoTest
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw "找不到 vswhere.exe，要先装 Visual Studio 2022 或 Build Tools" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw '找不到带 C++ 工具（VC.Tools.x86.x64）的 Visual Studio' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$ninjaPath = (Get-Command $Ninja -ErrorAction Stop).Source
$cmakePath = (Get-Command $CMake -ErrorAction Stop).Source

$build = Join-Path $Out 'build'
New-Item -ItemType Directory -Force $build | Out-Null
$steps = @(
    "`"$cmakePath`" -S `"$PSScriptRoot`" -B `"$build`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninjaPath`" -DCMAKE_BUILD_TYPE=$Config",
    "`"$cmakePath`" --build `"$build`""
)
if (-not $NoTest) { $steps += "ctest --test-dir `"$build`" --output-on-failure" }
$cmd = "call `"$vcvars`" >nul && " + ($steps -join ' && ')
# ctest sits next to cmake; vcvars64.bat looks for vswhere.exe on PATH.
$env:PATH = (Split-Path $cmakePath) + ';' + (Split-Path $vswhere) + ';' + $env:PATH
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "构建或测试失败（$LASTEXITCODE）" }

foreach ($name in 'modavd_extras.dll', 'external_renderer_ipc.dll') {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination (Join-Path $Out $name) -Force
    $hash = (Get-FileHash -LiteralPath (Join-Path $Out $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "$name  $hash"
}
