# SPDX-License-Identifier: GPL-3.0-or-later
# Local AX211 identity and optional firmware-buffer test. This is not an AirDrop installer.
# Temporarily replaces one exact disconnected PCI device's driver.
[CmdletBinding()]
param(
    [ValidateSet('Probe','Restore')][string]$Mode='Probe',
    [Parameter(Mandatory)][string]$StatePath,
    [string]$BuildDirectory,
    [ValidateSet('UMDF','KMDF')][string]$DriverKind='UMDF',
    [int]$DelaySeconds=0,
    [string]$FirmwarePath,
    [switch]$PowerCycle,
    [switch]$BootFirmware,
    [switch]$InitializeFirmware,
    [switch]$MonitorReceive,
    [switch]$TransmitFrame,
    [switch]$SessionTest,
    [switch]$OwlyRadio,
    [switch]$IbssRadio,
    [switch]$ReceiverTest,
    [switch]$AppSession,
    [ValidateLength(1,63)][string]$DeviceName="Windows PC",
    [string]$ReceiverPython,
    [string]$WintunPath,
    [string]$PnvmPath
)
$ErrorActionPreference='Stop'
if($AppSession) { $ReceiverTest=$true }
if($ReceiverTest) { $IbssRadio=$true }
if($IbssRadio) { $OwlyRadio=$true }
if($MonitorReceive -or $TransmitFrame -or $SessionTest -or $OwlyRadio) { $InitializeFirmware=$true }
$taskPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this hardware test as Administrator.'
}
$StatePath = [IO.Path]::GetFullPath($StatePath)
$taskStateDir = Split-Path -Parent $StatePath
$null = New-Item -ItemType Directory -Path $taskStateDir -Force
if (-not ('OpenDropDriverInstall' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OpenDropDriverInstall {
    [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool UpdateDriverForPlugAndPlayDevices(
        IntPtr parent, string hardware, string inf, uint flags, out bool reboot);
}
'@
}
function Save-State($Value) {
    $Value | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath "$StatePath.tmp" -Encoding UTF8
    Move-Item -LiteralPath "$StatePath.tmp" -Destination $StatePath -Force
}
function Device-Value([string]$Instance,[string]$Key) {
    (Get-PnpDeviceProperty -InstanceId $Instance -KeyName $Key -ErrorAction Stop).Data
}
function Install-ExactDriver([string]$Hardware,[string]$Inf) {
    $taskReboot = $false
    if (-not [OpenDropDriverInstall]::UpdateDriverForPlugAndPlayDevices(
        [IntPtr]::Zero,$Hardware,$Inf,5,[ref]$taskReboot)) {
        $taskError=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Driver installation failed with Win32 error $taskError (0x$($taskError.ToString('X8')))."
    }
    return $taskReboot
}
if ($Mode -eq 'Restore') {
    if ($DelaySeconds -gt 0) {
        $taskDeadline=(Get-Date).AddSeconds($DelaySeconds)
        while ($true) {
            if (Test-Path -LiteralPath $StatePath) {
                $taskPoll=Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
                if ($taskPoll.Completed) { exit 0 }
            }
            if((Get-Date) -ge $taskDeadline) {
                $taskLease=Join-Path $taskStateDir 'heartbeat'
                $taskLeaseFresh=$taskPoll.AppSession -and (Test-Path -LiteralPath $taskLease) -and
                    ([DateTime]::UtcNow-(Get-Item -LiteralPath $taskLease).LastWriteTimeUtc).TotalSeconds -lt 50
                if(-not $taskLeaseFresh) { break }
            }
            Start-Sleep -Seconds 3
        }
    }
    $taskMutex = New-Object Threading.Mutex($false,'Local\OpenDropAx211Restore')
    $taskHeld=$false
    try {
        $taskHeld=$taskMutex.WaitOne(60000)
        if (-not $taskHeld) { throw 'Another restoration is still running.' }
        $taskState=Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
        if ($taskState.Completed) { exit 0 }
        if($taskState.FirewallRulePrefix) {
            if($taskState.FirewallRulePrefix -notmatch '^OpenDrop-Test-[0-9a-f]{32}$') {
                throw 'Invalid saved firewall rule prefix; manual inspection is required.'
            }
            foreach($taskSuffix in @('tcp','mdns-in','mdns-out','nd-in','nd-out')) {
                $taskRule="$($taskState.FirewallRulePrefix)-$taskSuffix"
                Get-NetFirewallRule -Name $taskRule -ErrorAction SilentlyContinue |
                    Remove-NetFirewallRule -ErrorAction Stop
            }
        }
        $taskCurrent=Device-Value $taskState.Instance 'DEVPKEY_Device_DriverInfPath'
        if($taskState.UnsafeFirmwareState -and $taskState.StartedBootUtc) {
            $taskCurrentBoot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
            if($taskCurrentBoot -ne $taskState.StartedBootUtc) {
                # The driver never starts firmware in PrepareHardware/D0Entry.
                # A full restart ends the previous DMA session; permit recovery
                # even if test signing is now off and the prototype cannot load.
                $taskState.UnsafeFirmwareState=$false
                $taskState.RequiresRestart=$false
                Save-State $taskState
            }
        }
        if ($taskCurrent -ne $taskState.OriginalInf -and $taskState.UnsafeFirmwareState) {
            # Never unload a driver whose device might still be using DMA buffers.
            $taskHealthPath=Join-Path $taskStateDir 'restore-health.json'
            & "$($taskState.BuildDirectory)\ax211-probe.exe" --require-dma --output $taskHealthPath
            $taskHealthExit=$LASTEXITCODE
            $taskHealth=$null
            if(Test-Path -LiteralPath $taskHealthPath) {
                $taskHealth=Get-Content -LiteralPath $taskHealthPath -Raw | ConvertFrom-Json
            }
            if($taskHealthExit -ne 0 -or -not $taskHealth.ok -or
                $null -eq $taskHealth.firmware_dma_active -or $taskHealth.firmware_dma_active) {
                $taskState.RequiresRestart=$true
                Save-State $taskState
                throw 'DMA shutdown could not be verified. Development driver and buffers retained; a user-approved restart is required before restoration.'
            }
            $taskState.UnsafeFirmwareState=$false
            Save-State $taskState
        }
        if ($taskCurrent -ne $taskState.OriginalInf) {
            $taskState.RestoreNeedsReboot=Install-ExactDriver $taskState.Hardware $taskState.OriginalInfPath
        }
        $taskCurrent=Device-Value $taskState.Instance 'DEVPKEY_Device_DriverInfPath'
        $taskProblem=Device-Value $taskState.Instance 'DEVPKEY_Device_ProblemCode'
        if ($taskCurrent -ne $taskState.OriginalInf -or $taskProblem -ne 0) {
            throw "Original driver restoration not verified: INF=$taskCurrent Problem=$taskProblem."
        }
        $taskPackages=Get-WindowsDriver -Online -All | Where-Object {
            (Split-Path -Leaf $_.OriginalFileName) -eq ($taskState.PackageName + '.inf')
        }
        foreach($taskPackage in $taskPackages) {
            & pnputil.exe /delete-driver $taskPackage.Driver | Out-File -Append -LiteralPath "$taskStateDir\cleanup.log"
            if ($LASTEXITCODE -ne 0) { throw "Could not remove test driver package $($taskPackage.Driver)." }
        }
        if ($taskState.CertificateThumbprint) {
            if($taskState.CertificateThumbprint -notmatch '^[A-Fa-f0-9]{40}$') { throw 'Invalid saved certificate thumbprint.' }
            $taskCleanupStores=if($null -ne $taskState.CertificateStores) { @($taskState.CertificateStores) } else {
                @('Cert:\LocalMachine\Root','Cert:\LocalMachine\TrustedPublisher','Cert:\CurrentUser\My')
            }
            foreach($taskStore in $taskCleanupStores) {
                if($taskStore -notin @('Cert:\LocalMachine\Root','Cert:\LocalMachine\TrustedPublisher','Cert:\CurrentUser\My')) {
                    throw 'Invalid saved certificate store.'
                }
                $taskCertPath=Join-Path $taskStore $taskState.CertificateThumbprint
                if(Test-Path -LiteralPath $taskCertPath) { Remove-Item -LiteralPath $taskCertPath -Force }
            }
        }
        $taskState.Completed=$true
        $taskState.RestoredInf=$taskCurrent
        $taskState.RestoredProblemCode=$taskProblem
        $taskState.FinishedUtc=[DateTime]::UtcNow.ToString('o')
        Save-State $taskState
    } catch {
        $_ | Out-String | Add-Content -LiteralPath "$taskStateDir\restore-errors.log"
        throw
    } finally {
        if($taskHeld) { $taskMutex.ReleaseMutex() }
        $taskMutex.Dispose()
    }
    exit 0
}
$taskSessionMutex=New-Object Threading.Mutex($false,'Local\OpenDropAx211Session')
try { $taskSessionHeld=$taskSessionMutex.WaitOne(0) }
catch [Threading.AbandonedMutexException] { $taskSessionHeld=$true }
if(-not $taskSessionHeld) { throw 'Another OpenDrop radio session is already active.' }
try {
function Assert-AppLease {
    if($AppSession) {
        $taskLease=Join-Path $taskStateDir 'heartbeat'
        if((Test-Path -LiteralPath (Join-Path $taskStateDir 'heartbeat.stop')) -or
            -not(Test-Path -LiteralPath $taskLease) -or
            ([DateTime]::UtcNow-(Get-Item -LiteralPath $taskLease).LastWriteTimeUtc).TotalSeconds -gt 20) {
            throw 'The desktop app canceled this session or is no longer responding.'
        }
    }
}
Assert-AppLease
if(Test-Path -LiteralPath $StatePath) { throw 'Use a fresh state path for each test.' }
$BuildDirectory=(Resolve-Path -LiteralPath $BuildDirectory).Path
$taskPackageName=if($DriverKind -eq 'KMDF') {'opendrop_ax211_km'} else {'opendrop_ax211'}
$taskBinaryExtension=if($DriverKind -eq 'KMDF') {'sys'} else {'dll'}
$taskRelease=$null
if(Test-Path -LiteralPath "$BuildDirectory\release-driver.json") {
    if($DriverKind -ne 'KMDF') { throw 'The prebuilt package only supports KMDF.' }
    . "$PSScriptRoot\Release-Driver.ps1"
    $taskRelease=Get-ReleaseDriverMetadata $BuildDirectory
} else {
    $taskBuildInfo=Get-Content -LiteralPath "$BuildDirectory\build.json" -Raw | ConvertFrom-Json
    $taskInf2Cat=Join-Path $taskBuildInfo.WdkRoot 'bin\10.0.26100.0\x86\Inf2Cat.exe'
    if(-not (Test-Path -LiteralPath $taskInf2Cat)) { throw 'The WDK used by this build is no longer available.' }
}
if($DriverKind -eq 'KMDF') {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OpenDropCI {
 [StructLayout(LayoutKind.Sequential)] public struct Info { public uint Length; public uint Options; }
 [DllImport("ntdll.dll")] public static extern int NtQuerySystemInformation(int which, ref Info data, uint len, out uint used);
}
'@
    $taskCI=New-Object OpenDropCI+Info; $taskCI.Length=8; $taskCILength=0
    $taskCIStatus=[OpenDropCI]::NtQuerySystemInformation(103,[ref]$taskCI,8,[ref]$taskCILength)
    if($taskCIStatus -ne 0 -or -not ($taskCI.Options -band 2)) {
        throw 'Kernel prototype requires Windows driver test mode after a reboot. No driver or boot settings were changed.'
    }
}
if($PowerCycle -and ($DriverKind -ne 'KMDF' -or -not (Test-Path -LiteralPath "$BuildDirectory\ax211-stage.exe"))) {
    throw 'Power-cycle testing requires KMDF and its development client.'
}
if(($BootFirmware -or $InitializeFirmware) -and (-not $FirmwarePath -or $DriverKind -ne 'KMDF')) {
    throw 'Firmware boot requires the verified firmware image and the kernel driver.'
}
if($FirmwarePath) {
    if($DriverKind -ne 'KMDF') { throw 'Firmware staging requires the kernel driver.' }
    $FirmwarePath=(Resolve-Path -LiteralPath $FirmwarePath).Path
    $taskFirmwareHash=(Get-FileHash -LiteralPath $FirmwarePath -Algorithm SHA256).Hash
    if($taskFirmwareHash -ne '87590ea9535a8939fddb296bc7622f3a900f28f47b45cb4e435a2a1d5f496ccd') {
        throw 'Firmware is not the verified SO/GF API-77 development image.'
    }
    if(-not(Test-Path -LiteralPath "$BuildDirectory\ax211-stage.exe")) { throw 'Firmware staging client is missing.' }
}
if($OwlyRadio -and (-not $WintunPath -or
    -not (Test-Path -LiteralPath $WintunPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath "$BuildDirectory\radio-check.exe" -PathType Leaf))) {
    throw 'The live Owly test requires the verified Wintun DLL and radio-check.exe.'
}
if($OwlyRadio) {
    $WintunPath=(Resolve-Path -LiteralPath $WintunPath).Path
    if((Get-FileHash -LiteralPath $WintunPath -Algorithm SHA256).Hash -ne
        'e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce') {
        throw 'The live Owly test requires the official pinned Wintun 0.14.1 AMD64 DLL.'
    }
}
if($InitializeFirmware) {
    if($BootFirmware) { throw 'Choose one firmware test per installation.' }
    if(-not $PnvmPath) { throw 'Initialization test requires the authenticated PNVM image.' }
    $PnvmPath=(Resolve-Path -LiteralPath $PnvmPath).Path
    if((Get-FileHash -LiteralPath $PnvmPath -Algorithm SHA256).Hash -ne
        'b37f038b3467ed568d107a9c1dfd916fa7e8cbd58b2f9bec0c2c53297ccfcab7') {
        throw 'PNVM is not the authenticated SO/GF calibration image.'
    }
}
if($ReceiverTest) {
    if(-not $ReceiverPython -or -not(Test-Path -LiteralPath $ReceiverPython -PathType Leaf) -or
        -not(Test-Path -LiteralPath "$BuildDirectory\awdl-host.exe" -PathType Leaf)) {
        throw 'Receiver testing requires the native host and the unelevated receiver Python executable.'
    }
    $ReceiverPython=(Resolve-Path -LiteralPath $ReceiverPython).Path
}
$taskHardware='PCI\VEN_8086&DEV_7A70&SUBSYS_00948086&REV_11'
$taskDevices=@(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$taskHardware\*" })
if($taskDevices.Count -ne 1) { throw "Expected one exact AX211, found $($taskDevices.Count)." }
$taskDevice=$taskDevices[0]
$taskAdapter=@(Get-NetAdapter -IncludeHidden | Where-Object { $_.PnPDeviceID -eq $taskDevice.InstanceId })
if($taskAdapter.Count -ne 1 -or $taskAdapter[0].Status -ne 'Disconnected') {
    throw 'The target Wi-Fi adapter must be disconnected before this test.'
}
if(-not (Get-NetAdapter | Where-Object { $_.Status -eq 'Up' -and $_.PhysicalMediaType -eq '802.3' })) {
    throw 'A working Ethernet adapter is required to retain connectivity.'
}
$taskExisting=@(Get-WindowsDriver -Online -All | Where-Object {
    (Split-Path -Leaf $_.OriginalFileName) -in @('opendrop_ax211.inf','opendrop_ax211_km.inf')
})
if($taskExisting.Count) { throw 'An existing test package must be restored/removed first.' }
$taskOriginalInf=Device-Value $taskDevice.InstanceId 'DEVPKEY_Device_DriverInfPath'
$taskOriginalVersion=Device-Value $taskDevice.InstanceId 'DEVPKEY_Device_DriverVersion'
$taskOriginalPath=Join-Path "$env:windir\INF" $taskOriginalInf
$taskBackup=Join-Path $taskStateDir 'original-driver'
$null=New-Item -ItemType Directory -Path $taskBackup -Force
& pnputil.exe /export-driver $taskOriginalInf $taskBackup | Out-File -LiteralPath "$taskStateDir\backup.log"
if($LASTEXITCODE -ne 0) { throw 'Original driver backup failed.' }
$taskState=[pscustomobject]@{
    PackageName=$taskPackageName; DriverKind=$DriverKind; Instance=$taskDevice.InstanceId; Hardware=$taskHardware; OriginalInf=$taskOriginalInf
    OriginalInfPath=$taskOriginalPath; OriginalVersion=$taskOriginalVersion; Backup=$taskBackup
    BuildDirectory=$BuildDirectory; MonitorReceive=[bool]$MonitorReceive; TransmitFrame=[bool]$TransmitFrame; SessionTest=[bool]$SessionTest; OwlyRadio=[bool]$OwlyRadio; IbssRadio=[bool]$IbssRadio; InitializeExitCode=-1; PnvmPath=$PnvmPath; UnsafeFirmwareState=$false; RequiresRestart=$false; BootExitCode=-1
    FirmwarePath=$FirmwarePath; FirmwareStageExitCode=-1; PowerCycleExitCode=-1; CertificateThumbprint=''; Completed=$false; RestoreNeedsReboot=$false; InstallNeedsReboot=$false
    RestoredInf=''; RestoredProblemCode=-1; StartedUtc=[DateTime]::UtcNow.ToString('o')
    StartedBootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    TestProblemCode=-1; TestProblemStatus=0
    CertificateStores=@()
    FinishedUtc=''; ProbeExitCode=-1; InstallError=''; WatchdogPid=0; TestInf=''
    AppSession=[bool]$AppSession; DeviceName=$DeviceName
    ReceiverTest=[bool]$ReceiverTest
    FirewallRulePrefix=$(if($ReceiverTest) {'OpenDrop-Test-'+[guid]::NewGuid().ToString('N')} else {''})
}
Save-State $taskState
$taskFailed=$true
try {
    if($taskRelease) {
        $taskState.CertificateThumbprint=$taskRelease.thumbprint
        $taskPublicCert=Join-Path $BuildDirectory 'release.cer'
        Save-State $taskState
    } else {
        $taskCertificate=New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=OpenDrop AX211 temporary hardware test' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddDays(3) -KeyExportPolicy NonExportable -HashAlgorithm SHA256
        $taskState.CertificateThumbprint=$taskCertificate.Thumbprint
        $taskState.CertificateStores=@('Cert:\CurrentUser\My')
        Save-State $taskState
        $taskPublicCert=Join-Path $taskStateDir 'test-signing.cer'
        $null=Export-Certificate -Cert $taskCertificate -FilePath $taskPublicCert
        $taskKit=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Windows Kits\10'
        $taskSignTool=Get-ChildItem -Path "$taskKit\bin\*\x64\signtool.exe" |
            Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
        if(-not $taskSignTool) { throw 'signtool not found.' }
        & $taskSignTool sign /sha1 $taskCertificate.Thumbprint /fd SHA256 "$BuildDirectory\$taskPackageName.$taskBinaryExtension"
        if($LASTEXITCODE) { throw 'DLL signing failed.' }
        # Catalog must be regenerated after signing changes the DLL's bytes.
        if(-not (Test-Path -LiteralPath $taskInf2Cat)) { throw 'Inf2Cat not found: build with the local WDK first.' }
        & $taskInf2Cat "/driver:$BuildDirectory" /os:10_X64 /uselocaltime
        if($LASTEXITCODE) { throw 'Signed DLL catalog generation failed.' }
        & $taskSignTool sign /sha1 $taskCertificate.Thumbprint /fd SHA256 "$BuildDirectory\$taskPackageName.cat"
        if($LASTEXITCODE) { throw 'Catalog signing failed.' }
        $taskState.CertificateStores=@('Cert:\CurrentUser\My')
        Save-State $taskState
    }
    $taskWatchArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $PSCommandPath + '"'),
        '-Mode','Restore','-StatePath',('"' + $StatePath + '"'),'-DelaySeconds','180')
    $taskWatch=Start-Process -FilePath "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $taskWatchArgs -WindowStyle Hidden -PassThru
    $taskState.WatchdogPid=$taskWatch.Id
    Save-State $taskState
    foreach($taskStore in @('Cert:\LocalMachine\Root','Cert:\LocalMachine\TrustedPublisher')) {
        if(-not(Test-Path -LiteralPath (Join-Path $taskStore $taskState.CertificateThumbprint))) {
            # Journal ownership before import so an interrupted import is recoverable.
            $taskState.CertificateStores=@($taskState.CertificateStores)+$taskStore
            Save-State $taskState
            $null=Import-Certificate -FilePath $taskPublicCert -CertStoreLocation $taskStore
        }
    }
    foreach($taskSigned in @("$taskPackageName.$taskBinaryExtension","$taskPackageName.cat")) {
        $taskSignature=Get-AuthenticodeSignature -LiteralPath (Join-Path $BuildDirectory $taskSigned)
        if($taskSignature.Status -ne 'Valid' -or
           $taskSignature.SignerCertificate.Thumbprint -ne $taskState.CertificateThumbprint) {
            throw "Driver signature verification failed: $taskSigned"
        }
    }
    Assert-AppLease
    $taskState.InstallNeedsReboot=Install-ExactDriver $taskHardware "$BuildDirectory\$taskPackageName.inf"
    $taskState.TestInf=Device-Value $taskDevice.InstanceId 'DEVPKEY_Device_DriverInfPath'
    Save-State $taskState
    if($taskState.InstallNeedsReboot) { throw 'The device requests a restart; the test will restore without rebooting.' }
    Start-Sleep -Seconds 2
    $taskState.TestProblemCode=Device-Value $taskDevice.InstanceId 'DEVPKEY_Device_ProblemCode'
    $taskState.TestProblemStatus=Device-Value $taskDevice.InstanceId 'DEVPKEY_Device_ProblemStatus'
    Save-State $taskState
    $taskProbeArgs=@('--output',"$taskStateDir\identity.json")
    if($DriverKind -eq 'KMDF') { $taskProbeArgs += '--require-dma' }
    & "$BuildDirectory\ax211-probe.exe" @taskProbeArgs
    $taskState.ProbeExitCode=$LASTEXITCODE
    Save-State $taskState
    if($taskState.ProbeExitCode) { throw "Hardware identity test failed with exit $($taskState.ProbeExitCode)." }
    if($PowerCycle) {
        foreach($taskCycle in 1..3) {
            & "$BuildDirectory\ax211-stage.exe" --power-cycle "$taskStateDir\power-cycle-$taskCycle.json"
            $taskState.PowerCycleExitCode=$LASTEXITCODE
            Save-State $taskState
            if($taskState.PowerCycleExitCode) { throw "Controller power-cycle test $taskCycle failed." }
        }
    }
    if($FirmwarePath) {
        & "$BuildDirectory\ax211-stage.exe" $FirmwarePath "$taskStateDir\firmware-stage.json"
        $taskState.FirmwareStageExitCode=$LASTEXITCODE
        Save-State $taskState
        if($taskState.FirmwareStageExitCode) { throw "Firmware preparation failed: $($taskState.FirmwareStageExitCode)." }
    }
    if($BootFirmware) {
        $taskState.UnsafeFirmwareState=$true
        Save-State $taskState
        & "$BuildDirectory\ax211-stage.exe" --boot-firmware $FirmwarePath "$taskStateDir\firmware-boot.json"
        $taskState.BootExitCode=$LASTEXITCODE
        $taskBoot=Get-Content -LiteralPath "$taskStateDir\firmware-boot.json" -Raw | ConvertFrom-Json
        if($taskBoot.buffers_released) { $taskState.UnsafeFirmwareState=$false }
        Save-State $taskState
        if($taskState.BootExitCode -ne 0 -or -not $taskBoot.ok) {
            throw "Firmware boot test failed: $($taskState.BootExitCode). See firmware-boot.json."
        }
    }
    if($InitializeFirmware) {
        $taskState.UnsafeFirmwareState=$true
        Save-State $taskState
        $taskInitSwitch=if($SessionTest) {'--session-test'} elseif($TransmitFrame) {'--transmit-frame'} elseif($MonitorReceive) {'--monitor-receive'} else {'--init-firmware'}
        if($ReceiverTest) {
            $taskReceiverArgs=@{RadioHost="$BuildDirectory\awdl-host.exe";FirmwarePath=$FirmwarePath;
                PnvmPath=$PnvmPath;WintunPath=$WintunPath;SessionDirectory=$taskStateDir;
                PythonProgram=$ReceiverPython;RulePrefix=$taskState.FirewallRulePrefix;
                AppSession=[bool]$AppSession;DeviceName=$DeviceName}
            $taskInitExit=& "$PSScriptRoot\Invoke-ReceiverWindow.ps1" @taskReceiverArgs
        } elseif($OwlyRadio) {
            $taskRadioMode=if($IbssRadio) {'ibss'} else {'monitor'}
            $taskRadioArgs=@(('"' + $FirmwarePath + '"'),('"' + $PnvmPath + '"'),
                ('"' + $WintunPath + '"'),'30',$taskRadioMode)
            $taskRun=@{FilePath="$BuildDirectory\radio-check.exe"; ArgumentList=$taskRadioArgs;
                OutputPath="$taskStateDir\firmware-init.json"; ErrorPath="$taskStateDir\owly-error.log";
                TimeoutSeconds=90}
            $taskInitExit=& "$PSScriptRoot\Invoke-BoundedTool.ps1" @taskRun
        } else {
            & "$BuildDirectory\ax211-stage.exe" $taskInitSwitch $FirmwarePath $PnvmPath "$taskStateDir\firmware-init.json"
            $taskInitExit=$LASTEXITCODE
        }
        $taskState.InitializeExitCode=$taskInitExit
        $taskInit=Get-Content -LiteralPath "$taskStateDir\firmware-init.json" -Raw | ConvertFrom-Json
        if($taskInit.buffers_released) { $taskState.UnsafeFirmwareState=$false }
        Save-State $taskState
        if($taskState.InitializeExitCode -ne 0 -or -not $taskInit.ok) {
            throw "Firmware initialization test failed: $($taskState.InitializeExitCode). See firmware-init.json."
        }
    }
    $taskFailed=$false
} catch {
    $taskState.InstallError=$_.Exception.Message
    Save-State $taskState
    $_ | Out-String | Out-File -LiteralPath "$taskStateDir\test-error.log"
} finally {
    & $PSCommandPath -Mode Restore -StatePath $StatePath
}
if($taskFailed) { exit 1 }

} finally { if($taskSessionHeld) { $taskSessionMutex.ReleaseMutex() }; $taskSessionMutex.Dispose() }
