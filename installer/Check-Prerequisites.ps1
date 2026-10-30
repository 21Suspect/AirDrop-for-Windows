# SPDX-License-Identifier: GPL-3.0-or-later
# Read-only detection. Writes a report, never installs or changes boot settings.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputPath,[switch]$CheckRemoval)
$ErrorActionPreference='Stop'
function Save-Report($Values) {
    $lines=@('[checks]')
    foreach($key in $Values.Keys) {
        $value=([string]$Values[$key]) -replace '[\r\n]',' '
        $lines+=("$key=$value")
    }
    $lines | Set-Content -LiteralPath $OutputPath -Encoding Unicode
}
$result=[ordered]@{supported='0';hardware='Unable to inspect hardware';testmode='Unknown';secureboot='Unknown';ethernet='Unknown';bluetooth='Unknown';wifi='Disconnect Wi-Fi before turning on AirDrop';removable='0';message=''}
try {
    if($CheckRemoval) {
        $journal=Join-Path $env:LOCALAPPDATA 'OpenDrop\session.json'
        if(Test-Path -LiteralPath $journal) {
            $session=Get-Content -LiteralPath $journal -Raw -Encoding UTF8 | ConvertFrom-Json
            $state=Join-Path $session.path 'state.json'
            if(-not(Test-Path -LiteralPath $state)) { throw 'A radio session has no confirmed restoration record. Open the app and complete recovery before uninstalling.' }
            $record=Get-Content -LiteralPath $state -Raw -Encoding UTF8 | ConvertFrom-Json
            if(-not $record.Completed) { throw 'Restore Wi-Fi in the app before uninstalling. The saved radio session is unfinished.' }
        }
        $result.removable='1'
    } else {
        $version=[Environment]::OSVersion.Version
        $nativeArchitecture=if($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
        $devices=@(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_7A70&SUBSYS_00948086&REV_11\*' })
        if($devices.Count -eq 1 -and $version.Build -ge 26100 -and [Environment]::Is64BitOperatingSystem -and
           $nativeArchitecture -eq 'AMD64') {
            $result.supported='1';$result.hardware='Supported Intel AX211 found'
        } else {
            $result.hardware='Unsupported configuration'
            $result.message='Requires Windows 11 24H2 or newer on x64 and exactly one AX211: PCI 8086:7A70 / subsystem 00948086 / revision 11. Other adapters cannot be enabled by this installer.'
        }
        # The setup check reads current boot options without runtime C# compilation.
        # The elevated radio helper independently enforces NtQuerySystemInformation.
        $bootOptions=(Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions).SystemStartOptions
        if(-not $bootOptions) { throw 'Cannot read the current boot configuration.' }
        $result.testmode=if($bootOptions -match '(?i)(^|\s)TESTSIGNING(\s|$)') {'Enabled'} else {'Not enabled - restart required after preparation'}
        try {
            $secure=(Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled).UEFISecureBootEnabled
            $result.secureboot=if($secure -eq 1) {'On - firmware change required for Test Mode'} else {'Off'}
        } catch { $result.secureboot='Unknown - inspect firmware settings before Test Mode preparation' }
        $adapters=@(Get-NetAdapter -IncludeHidden)
        $wired=@($adapters | Where-Object { $_.Status -eq 'Up' -and $_.PhysicalMediaType -eq '802.3' })
        $result.ethernet=if($wired.Count) {'Connected'} else {'Connect Ethernet before turning on AirDrop'}
        $bluetooth=@(Get-PnpDevice -Class Bluetooth -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'OK' })
        $result.bluetooth=if($bluetooth.Count) {'Detected - keep Bluetooth on'} else {'No working Bluetooth device detected'}
        if(@($devices).Count -eq 1) {
            $wifi=@($adapters | Where-Object { $_.PnPDeviceID -eq $devices[0].InstanceId })
            if($wifi.Count -eq 1 -and $wifi[0].Status -eq 'Disconnected') { $result.wifi='Disconnected - ready for exclusive AirDrop use' }
        }
    }
} catch {
    $result.message=$_.Exception.Message
    if(-not $CheckRemoval) { $result.supported='0'; $result.hardware='Detection incomplete' }
}
Save-Report $result
