param(
    [string]$ZigPath = 'zig.exe',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutputPath,
    [switch]$WithIcon
)
$ErrorActionPreference = 'Stop'
$projectDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $BuildDirectory 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $BuildDirectory 'zig-local-cache'
$resourcePath = Join-Path $BuildDirectory 'CellBattery.res'
$resourceFile = Join-Path $PSScriptRoot 'CellBattery.rc'
if ($WithIcon) {
    $iconPath = Join-Path $projectDirectory 'previews\CellBattery.ico'
    if (-not (Test-Path -LiteralPath $iconPath)) { throw 'Run --render to create previews/CellBattery.ico first.' }
    $resourceText = [IO.File]::ReadAllText($resourceFile)
    $resourceText += "`r`n2 ICON `"$($iconPath.Replace('\','/'))`"`r`n"
    $resourceFile = Join-Path $BuildDirectory 'CellBattery-icon.rc'
    [IO.File]::WriteAllText($resourceFile, $resourceText)
}
Push-Location $PSScriptRoot
try {
    & $ZigPath rc /I $PSScriptRoot /fo $resourcePath $resourceFile
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    $executablePath = if ($OutputPath) { [IO.Path]::GetFullPath($OutputPath) } else { Join-Path $projectDirectory 'CellBattery.exe' }
    & $ZigPath c++ (Join-Path $PSScriptRoot 'CellBattery.cpp') $resourcePath '-std=c++17' '-Os' '-DNDEBUG' '-fno-exceptions' '-fno-rtti' '-ffunction-sections' '-fdata-sections' '-static' '-s' '-municode' '-Wl,--subsystem,windows' '-Wl,--gc-sections' '-luser32' '-lgdi32' '-lgdiplus' '-lcomctl32' '-lshell32' '-lole32' '-ladvapi32' '-ldwmapi' '-lpowrprof' '-lwtsapi32' '-luuid' '-lsetupapi' '-o' $executablePath
    if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed.' }
    Get-Item -LiteralPath $executablePath | Select-Object FullName,Length
} finally { Pop-Location }
