param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutputPath,
    [switch]$WithIcon,
    [switch]$EnvironmentOnly
)
$ErrorActionPreference='Stop'
$projectDirectory=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudioRoot=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $visualStudioRoot) { throw 'MSVC C++ Build Tools were not found. Use build.ps1 with Zig instead.' }
$msvcDirectory=Get-ChildItem -LiteralPath (Join-Path $visualStudioRoot 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$sdkRoot=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkDirectory=Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvcDirectory -or -not $sdkDirectory) { throw 'MSVC or Windows SDK headers are missing.' }
$env:INCLUDE=@((Join-Path $msvcDirectory.FullName 'include'),(Join-Path $sdkDirectory.FullName 'ucrt'),(Join-Path $sdkDirectory.FullName 'shared'),(Join-Path $sdkDirectory.FullName 'um'),(Join-Path $sdkDirectory.FullName 'winrt')) -join ';'
$env:LIB=@((Join-Path $msvcDirectory.FullName 'lib\x64'),(Join-Path $sdkRoot ('Lib\'+$sdkDirectory.Name+'\ucrt\x64')),(Join-Path $sdkRoot ('Lib\'+$sdkDirectory.Name+'\um\x64'))) -join ';'
$env:PATH=(Join-Path $msvcDirectory.FullName 'bin\Hostx64\x64')+';'+$env:PATH
if ($EnvironmentOnly) { return }
$resourcePath=Join-Path $BuildDirectory 'CellBattery.res'
$resourceText=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'CellBattery.rc'))
if ($WithIcon) { $iconPath=Join-Path $projectDirectory 'previews\CellBattery.ico';$resourceText+="`r`n2 ICON `"$($iconPath.Replace('\','/'))`"`r`n" }
$resourceFile=Join-Path $BuildDirectory 'CellBattery-msvc.rc'
[IO.File]::WriteAllText($resourceFile,$resourceText)
$resourceCompiler=Join-Path $sdkRoot ('bin\'+$sdkDirectory.Name+'\x64\rc.exe')
$executablePath=if ($OutputPath) { [IO.Path]::GetFullPath($OutputPath) } else { Join-Path $projectDirectory 'CellBattery.exe' }
& $resourceCompiler /nologo /I $PSScriptRoot /fo $resourcePath $resourceFile
if ($LASTEXITCODE -ne 0) { throw 'Windows resource compilation failed.' }
& cl.exe /nologo /std:c++17 /utf-8 /O1 /GL /MT /GR- /EHs-c- /D_HAS_EXCEPTIONS=0 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS "/Fo$(Join-Path $BuildDirectory 'CellBattery.obj')" "/Fe$executablePath" (Join-Path $PSScriptRoot 'CellBattery.cpp') $resourcePath /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO user32.lib gdi32.lib gdiplus.lib comctl32.lib shell32.lib ole32.lib advapi32.lib dwmapi.lib powrprof.lib wtsapi32.lib uuid.lib setupapi.lib
if ($LASTEXITCODE -ne 0) { throw 'MSVC compilation failed.' }
Get-Item -LiteralPath $executablePath | Select-Object FullName,Length
