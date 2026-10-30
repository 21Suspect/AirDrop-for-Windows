# SPDX-License-Identifier: GPL-3.0-or-later
# Repeat the verified development path in a fresh, bounded session.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DriverBuild,
    [Parameter(Mandatory)][string]$FirmwarePath,
    [Parameter(Mandatory)][string]$PnvmPath,
    [Parameter(Mandatory)][string]$WintunPath,
    [Parameter(Mandatory)][string]$Python,
    [string]$OutputDirectory=(Join-Path ([Environment]::GetFolderPath('MyPictures')) 'AirDrop')
)
$ErrorActionPreference='Stop'
$taskPrincipal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if($taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Start this launcher normally, without Run as administrator. It elevates only the radio helper.'
}
$DriverBuild=(Resolve-Path -LiteralPath $DriverBuild).Path
$FirmwarePath=(Resolve-Path -LiteralPath $FirmwarePath).Path
$PnvmPath=(Resolve-Path -LiteralPath $PnvmPath).Path
$WintunPath=(Resolve-Path -LiteralPath $WintunPath).Path
$Python=(Resolve-Path -LiteralPath $Python).Path
$taskBasePython=& $Python -c 'import sys; print(sys._base_executable)'
if($LASTEXITCODE -or -not(Test-Path -LiteralPath $taskBasePython -PathType Leaf)) {
    throw 'Python or its base executable could not be located.'
}
& $Python -c 'import cryptography, ifaddr, zeroconf'
if($LASTEXITCODE) { throw 'Install requirements-windows.txt into the chosen Python environment first.' }
$taskFiles=@('opendrop_ax211_km.sys','opendrop_ax211_km.inf','build.json',
    'ax211-probe.exe','ax211-stage.exe','awdl-host.exe','radio-check.exe')
foreach($taskFile in $taskFiles) {
    if(-not(Test-Path -LiteralPath (Join-Path $DriverBuild $taskFile) -PathType Leaf)) {
        throw "Missing compiled driver/helper: $taskFile"
    }
}
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskSession=Join-Path ([IO.Path]::GetTempPath()) ('opendrop-receive-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $taskSession
foreach($taskSub in @('kernel','test','receiver-source','receiver-source\tools')) {
    $null=New-Item -ItemType Directory -Path (Join-Path $taskSession $taskSub)
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'native\intel') -Destination (Join-Path $taskSession 'src') -Recurse
Copy-Item -LiteralPath (Join-Path $taskRoot 'opendrop') -Destination (Join-Path $taskSession 'receiver-source\opendrop') -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'receive_test.py') -Destination (Join-Path $taskSession 'receiver-source\tools\receive_test.py')
foreach($taskFile in $taskFiles) {
    Copy-Item -LiteralPath (Join-Path $DriverBuild $taskFile) -Destination (Join-Path $taskSession 'kernel')
}
$taskConfig=@{FirmwarePath=$FirmwarePath;PnvmPath=$PnvmPath;WintunPath=$WintunPath;ReceiverPython=$taskBasePython}
$taskConfig | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskSession 'config.json') -Encoding UTF8
$taskElevated=@'
$ErrorActionPreference='Stop'
Start-Transcript -Path (Join-Path $PSScriptRoot 'test-transcript.log') -Force
try {
    $config=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'config.json') -Raw | ConvertFrom-Json
    $parameters=@{StatePath=(Join-Path $PSScriptRoot 'test\state.json');
        BuildDirectory=(Join-Path $PSScriptRoot 'kernel');DriverKind='KMDF';ReceiverTest=$true;
        FirmwarePath=$config.FirmwarePath;PnvmPath=$config.PnvmPath;
        WintunPath=$config.WintunPath;ReceiverPython=$config.ReceiverPython}
    & (Join-Path $PSScriptRoot 'src\Test-IntelDriver.ps1') @parameters
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'launcher-error.log')
    @{ok=$false;error=$_.Exception.Message} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'test\receiver-ended.json') -Encoding UTF8
} finally { Stop-Transcript }
'@
$taskLaunchFile=Join-Path $taskSession 'run-radio.ps1'
$taskElevated | Set-Content -LiteralPath $taskLaunchFile -Encoding UTF8
Write-Host "Session logs: $taskSession"
Write-Host 'The receiver window lasts about two minutes after the radio starts.'
Write-Host "Incoming files are accepted automatically into $OutputDirectory."
$taskArguments='-NoProfile -ExecutionPolicy Bypass -File "'+$taskLaunchFile+'"'
$taskProcess=Start-Process "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -WindowStyle Hidden -ArgumentList $taskArguments -PassThru
try {
    & $Python -u (Join-Path $taskSession 'receiver-source\tools\receive_test.py') (Join-Path $taskSession 'test') --output $OutputDirectory
    $taskReceiverExit=$LASTEXITCODE
    $taskDeadline=[DateTime]::UtcNow.AddSeconds(90)
    $taskState=$null
    do {
        $taskStateFile=Join-Path $taskSession 'test\state.json'
        if(Test-Path -LiteralPath $taskStateFile) {
            $taskState=Get-Content -LiteralPath $taskStateFile -Raw | ConvertFrom-Json
            if($taskState.Completed) { break }
        } elseif($taskProcess.HasExited) { break }
        Start-Sleep -Milliseconds 500
    } while([DateTime]::UtcNow -lt $taskDeadline)
    if(-not $taskState.Completed -or $taskState.UnsafeFirmwareState -or
        $taskState.RestoredProblemCode -ne 0) {
        throw "Driver restoration needs inspection. Read $taskSession\test\state.json and launcher-error.log."
    }
    Write-Host "Test ended; restored $($taskState.RestoredInf). Received files remain in $OutputDirectory."
    if($taskReceiverExit) { throw "Receiver exited with code $taskReceiverExit; see session logs." }
} finally { $taskProcess.Dispose() }
