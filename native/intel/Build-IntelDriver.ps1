# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory)][string]$WdkRoot, [Parameter(Mandatory)][string]$OutDir)
$ErrorActionPreference = 'Stop'
$WdkRoot = (Resolve-Path -LiteralPath $WdkRoot).Path
$null = New-Item -ItemType Directory -Path $OutDir -Force
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$taskProgramFiles = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$taskVsWhere = "$taskProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
$taskVs = & $taskVsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'Install Visual Studio C++ Build Tools first.' }
$taskMsvc = Get-ChildItem -LiteralPath "$taskVs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$taskKit = "$taskProgramFiles\Windows Kits\10"
$taskSdk = Get-ChildItem -LiteralPath "$taskKit\Include" -Directory |
    Where-Object { Test-Path -LiteralPath "$($_.FullName)\um\windows.h" } |
    Sort-Object Name | Select-Object -Last 1 -ExpandProperty Name
if (-not $taskSdk) { throw 'Windows SDK not installed.' }
$taskCl = "$($taskMsvc.FullName)\bin\Hostx64\x64\cl.exe"
$taskIncludes = @("$($taskMsvc.FullName)\include", "$taskKit\Include\$taskSdk\ucrt",
    "$taskKit\Include\$taskSdk\um", "$taskKit\Include\$taskSdk\shared",
    "$WdkRoot\Include\wdf\umdf\2.33")
$taskLibraries = @("$($taskMsvc.FullName)\lib\x64", "$taskKit\Lib\$taskSdk\ucrt\x64",
    "$taskKit\Lib\$taskSdk\um\x64", "$WdkRoot\Lib\wdf\umdf\x64\2.33")
$taskOldInclude = $env:INCLUDE
$taskOldLib = $env:LIB
$taskOldPath = $env:PATH
try {
    $env:INCLUDE = $taskIncludes -join ';'
    $env:LIB = $taskLibraries -join ';'
    $env:PATH = "$(Split-Path -Parent $taskCl);$taskOldPath"
    $taskCommon = @('/nologo','/W4','/WX','/wd4324','/O2','/GS','/guard:cf','/Zi','/MT',
        '/DUNICODE','/D_UNICODE','/D_AMD64_','/DAMD64','/DWIN32_LEAN_AND_MEAN')
    & $taskCl @taskCommon /DUMDF_VERSION_MAJOR=2 /DUMDF_VERSION_MINOR=33 /LD "$PSScriptRoot\ax211_umdf.c" "/Fo$OutDir\ax211_umdf.obj" "/Fd$OutDir\ax211_umdf.pdb" /link "/OUT:$OutDir\opendrop_ax211.dll" /IMPLIB:"$OutDir\opendrop_ax211.lib" /PDB:"$OutDir\opendrop_ax211.pdb" /DLL /SUBSYSTEM:WINDOWS /GUARD:CF /DEBUG '/PDBALTPATH:%_PDB%' /INCREMENTAL:NO /OPT:REF /OPT:ICF WdfDriverStubUm.lib ntdll.lib mincore.lib
    if ($LASTEXITCODE) { throw "Driver compilation failed: $LASTEXITCODE" }
    & $taskCl @taskCommon "$PSScriptRoot\ax211_probe.c" "/Fo$OutDir\ax211_probe.obj" "/Fd$OutDir\ax211_probe.pdb" /link "/OUT:$OutDir\ax211-probe.exe" /GUARD:CF /DEBUG '/PDBALTPATH:%_PDB%' /INCREMENTAL:NO setupapi.lib cfgmgr32.lib
    if ($LASTEXITCODE) { throw "Probe compilation failed: $LASTEXITCODE" }
    foreach($taskUnit in @('ax211_stage','iwl_firmware')) {
        & $taskCl @taskCommon /c "$PSScriptRoot\$taskUnit.c" "/Fo$OutDir\$taskUnit.obj" "/Fd$OutDir\$taskUnit.pdb"
        if($LASTEXITCODE) { throw "Firmware tool compilation failed: $LASTEXITCODE" }
    }
    & $taskCl /nologo "$OutDir\ax211_stage.obj" "$OutDir\iwl_firmware.obj" /link "/OUT:$OutDir\ax211-stage.exe" /GUARD:CF /DEBUG '/PDBALTPATH:%_PDB%' /INCREMENTAL:NO setupapi.lib cfgmgr32.lib
    if($LASTEXITCODE) { throw "Firmware tool linking failed: $LASTEXITCODE" }
    Copy-Item -LiteralPath "$PSScriptRoot\opendrop_ax211.inf" -Destination $OutDir
    & "$WdkRoot\tools\10.0.26100.0\x64\infverif.exe" /w "$OutDir\opendrop_ax211.inf"
    if ($LASTEXITCODE) { throw "INF validation failed: $LASTEXITCODE" }
    & "$WdkRoot\bin\10.0.26100.0\x86\Inf2Cat.exe" "/driver:$OutDir" /os:10_X64 /uselocaltime
    if ($LASTEXITCODE) { throw "Catalog generation failed: $LASTEXITCODE" }
    [pscustomobject]@{Compiler=$taskCl; Sdk=$taskSdk; Umdf='2.33'; WdkRoot=$WdkRoot; Output=$OutDir} |
        ConvertTo-Json | Set-Content -LiteralPath "$OutDir\build.json"
}
finally {
    $env:INCLUDE = $taskOldInclude
    $env:LIB = $taskOldLib
    $env:PATH = $taskOldPath
}
