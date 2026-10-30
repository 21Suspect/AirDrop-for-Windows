# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Collect read-only Windows radio evidence for an OpenDrop port investigation.
.DESCRIPTION
Does not change adapters, drivers, firewall rules, certificates or radio modes.
Does not enumerate nearby networks, saved profiles, MAC addresses or paired peers.
Exit code zero means collection succeeded, not that AirDrop works.
.PARAMETER ReportPath
Optional JSON destination. Its parent directory must already exist.
#>
[CmdletBinding()]
param([string]$ReportPath)

$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This collector requires Windows.'
}
$taskOs = Get-CimInstance Win32_OperatingSystem |
    Select-Object Caption, Version, OSArchitecture
$taskAdapters = @(Get-NetAdapter -Physical |
    Select-Object InterfaceDescription, MediaType, PhysicalMediaType,
                  DriverVersion, DriverDate, Status)
$taskBluetooth = @(Get-PnpDevice -PresentOnly -Class Bluetooth |
    Where-Object { $_.InstanceId -like 'USB\*' } |
    Select-Object FriendlyName, Status)
$taskCapabilities = (& "$env:SystemRoot\System32\netsh.exe" wlan show wirelesscapabilities 2>&1 |
    Out-String).Trim()
$taskNetshExitCode = $LASTEXITCODE
if ($taskNetshExitCode -ne 0) {
    throw "netsh failed with exit code ${taskNetshExitCode}: $taskCapabilities"
}
$taskReport = [ordered]@{
    SchemaVersion = 1
    CollectedAt = [DateTimeOffset]::Now.ToString('o')
    OperatingSystem = $taskOs
    PhysicalNetworkAdapters = $taskAdapters
    BluetoothUsbRadios = $taskBluetooth
    WirelessCapabilities = $taskCapabilities
    Interpretation = @(
        'Capability inventory only; not an AirDrop compatibility test.'
        'OWL needs raw 802.11 receive/transmit, channel control and an AWDL interface.'
        'Monitor-mode support alone would not prove frame injection or AWDL compatibility.'
        'Wi-Fi Direct and Bluetooth presence do not establish AWDL support.'
        'Localized netsh text is preserved without guessing support from translated labels.'
    )
}
$taskJson = $taskReport | ConvertTo-Json -Depth 6
if ($ReportPath) {
    $taskOutput = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ReportPath)
    [IO.File]::WriteAllText($taskOutput, $taskJson, [Text.UTF8Encoding]::new($false))
    Write-Output "Saved radio evidence to $taskOutput"
} else {
    Write-Output $taskJson
}