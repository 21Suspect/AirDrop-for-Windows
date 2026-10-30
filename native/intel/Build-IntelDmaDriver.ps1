# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory)][string]$WdkRoot,[Parameter(Mandatory)][string]$OutDir)
$ErrorActionPreference='Stop'
$WdkRoot=(Resolve-Path -LiteralPath $WdkRoot).Path
$null=New-Item -ItemType Directory -Path $OutDir -Force
$OutDir=(Resolve-Path -LiteralPath $OutDir).Path
$taskPF=[Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$taskVs=& "$taskPF\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $taskVs) { throw 'C++ Build Tools not installed.' }
$taskMsvc=Get-ChildItem -LiteralPath "$taskVs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$taskKit="$taskPF\Windows Kits\10"
$taskSdk=Get-ChildItem -LiteralPath "$taskKit\Include" -Directory |
    Where-Object { Test-Path -LiteralPath "$($_.FullName)\shared\ntdef.h" } |
    Sort-Object Name | Select-Object -Last 1 -ExpandProperty Name
$taskCl="$($taskMsvc.FullName)\bin\Hostx64\x64\cl.exe"
$taskLink="$($taskMsvc.FullName)\bin\Hostx64\x64\link.exe"
$taskOldInclude=$env:INCLUDE; $taskOldLib=$env:LIB
try {
    $env:INCLUDE=@("$WdkRoot\Include\10.0.26100.0\km","$WdkRoot\Include\wdf\kmdf\1.33",
        "$taskKit\Include\$taskSdk\shared","$taskKit\Include\$taskSdk\ucrt","$($taskMsvc.FullName)\include") -join ';'
    $env:LIB=@("$WdkRoot\Lib\10.0.26100.0\km\x64","$WdkRoot\Lib\wdf\kmdf\x64\1.33") -join ';'
    & $taskCl /nologo /c /W4 /WX /wd4324 /kernel /O2 /GS /guard:cf /Zl /Zi /D_AMD64_ /DAMD64 /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 "$PSScriptRoot\ax211_kmdf.c" "/Fo$OutDir\ax211_kmdf.obj" "/Fd$OutDir\ax211_kmdf.pdb"
    if($LASTEXITCODE) { throw "Kernel compilation failed: $LASTEXITCODE" }
    & $taskCl /nologo /c /W4 /WX /kernel /O2 /GS /guard:cf /Zl /Zi /D_AMD64_ /DAMD64 "$PSScriptRoot\iwl_firmware.c" "/Fo$OutDir\iwl_firmware.obj" "/Fd$OutDir\iwl_firmware.pdb"
    if($LASTEXITCODE) { throw "Firmware parser compilation failed: $LASTEXITCODE" }
    & $taskCl /nologo /c /W4 /WX /kernel /O2 /GS /guard:cf /Zl /Zi /D_AMD64_ /DAMD64 "$PSScriptRoot\iwl_hw.c" "/Fo$OutDir\iwl_hw.obj" "/Fd$OutDir\iwl_hw.pdb"
    if($LASTEXITCODE) { throw "Hardware sequence compilation failed: $LASTEXITCODE" }
    & $taskCl /nologo /c /std:c11 /W4 /WX /kernel /O2 /GS /guard:cf /Zl /Zi /D_AMD64_ /DAMD64 "$PSScriptRoot\iwl_gen3.c" "/Fo$OutDir\iwl_gen3.obj" "/Fd$OutDir\iwl_gen3.pdb"
    if($LASTEXITCODE) { throw "DMA layout compilation failed: $LASTEXITCODE" }
    foreach($taskUnit in @('iwl_metadata','iwl_response','iwl_pnvm','iwl_command','iwl_packet','iwl_radio')) {
        & $taskCl /nologo /c /std:c11 /W4 /WX /kernel /O2 /GS /guard:cf /Zl /Zi /D_AMD64_ /DAMD64 "$PSScriptRoot\$taskUnit.c" "/Fo$OutDir\$taskUnit.obj" "/Fd$OutDir\$taskUnit.pdb"
        if($LASTEXITCODE) { throw "Initialization component compilation failed: $LASTEXITCODE" }
    }
    & $taskLink /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:FxDriverEntry /NODEFAULTLIB /INTEGRITYCHECK /DYNAMICBASE /NXCOMPAT /RELEASE /GUARD:CF /DEBUG '/PDBALTPATH:%_PDB%' /INCREMENTAL:NO /OPT:REF /OPT:ICF "/OUT:$OutDir\opendrop_ax211_km.sys" "/PDB:$OutDir\opendrop_ax211_km.pdb" "$OutDir\ax211_kmdf.obj" "$OutDir\iwl_firmware.obj" "$OutDir\iwl_hw.obj" "$OutDir\iwl_gen3.obj" "$OutDir\iwl_metadata.obj" "$OutDir\iwl_response.obj" "$OutDir\iwl_pnvm.obj" "$OutDir\iwl_command.obj" "$OutDir\iwl_packet.obj" "$OutDir\iwl_radio.obj" WdfDriverEntry.lib WdfLdr.lib ntoskrnl.lib hal.lib BufferOverflowFastFailK.lib
    if($LASTEXITCODE) { throw "Kernel linking failed: $LASTEXITCODE" }
    Copy-Item -LiteralPath "$PSScriptRoot\opendrop_ax211_km.inf" -Destination $OutDir
    & "$WdkRoot\tools\10.0.26100.0\x64\infverif.exe" /w "$OutDir\opendrop_ax211_km.inf"
    if($LASTEXITCODE) { throw "INF verification failed: $LASTEXITCODE" }
    & "$WdkRoot\bin\10.0.26100.0\x86\Inf2Cat.exe" "/driver:$OutDir" /os:10_X64 /uselocaltime
    if($LASTEXITCODE) { throw "Catalog generation failed: $LASTEXITCODE" }
    [pscustomobject]@{Compiler=$taskCl;Sdk=$taskSdk;WdkRoot=$WdkRoot;Kmdf='1.33';HardwareTested=$false} |
        ConvertTo-Json | Set-Content -LiteralPath "$OutDir\build.json"
} finally { $env:INCLUDE=$taskOldInclude; $env:LIB=$taskOldLib }
