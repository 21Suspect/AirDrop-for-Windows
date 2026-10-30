# SPDX-License-Identifier: GPL-3.0-or-later
# Bounded native radio; file reception runs in a separate unelevated process.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RadioHost,
    [Parameter(Mandatory)][string]$FirmwarePath,
    [Parameter(Mandatory)][string]$PnvmPath,
    [Parameter(Mandatory)][string]$WintunPath,
    [Parameter(Mandatory)][string]$SessionDirectory,
    [Parameter(Mandatory)][string]$PythonProgram,
    [Parameter(Mandatory)][ValidatePattern('^OpenDrop-Test-[0-9a-f]{32}$')][string]$RulePrefix,
    [ValidateRange(10,120)][int]$Seconds=120,
    [switch]$AppSession,
    [ValidateLength(1,63)][string]$DeviceName="Windows PC"
)
$ErrorActionPreference='Stop'
$taskProcess=$null
$taskResult=[ordered]@{ok=$false;buffers_released=$false;iphone_transfer_verified=$false}
$taskSuffixes=@('tcp','mdns-in','mdns-out','nd-in','nd-out')
$taskRawReady=Join-Path $SessionDirectory 'radio-ready.json'
foreach($taskPath in @($RadioHost,$FirmwarePath,$PnvmPath,$WintunPath,$SessionDirectory,$PythonProgram,$DeviceName)) {
    if($taskPath.Contains('"') -or $taskPath.Contains([char]10) -or $taskPath.Contains([char]13)) {
        throw 'Development helper paths must not contain quotes or line breaks.'
    }
}
try {
    $taskLease=Join-Path $SessionDirectory 'heartbeat'
    if($AppSession) {
        if(-not(Test-Path -LiteralPath $taskLease) -or
            ([DateTime]::UtcNow-(Get-Item -LiteralPath $taskLease).LastWriteTimeUtc).TotalSeconds -gt 20) {
            throw 'The app is no longer connected; no radio was started.'
        }
        $taskLifetime=@('--leased',('"'+$taskLease+'"'),('"'+$taskRawReady+'"'))
    } else { $taskLifetime=@('--bounded',[string]$Seconds,('"'+$taskRawReady+'"')) }
    $taskArgs=@(('"'+$FirmwarePath+'"'),('"'+$PnvmPath+'"'),('"'+$WintunPath+'"'),
        ('"'+$DeviceName+'"')) + $taskLifetime
    $taskStart=@{FilePath=$RadioHost;ArgumentList=$taskArgs;WindowStyle='Hidden';PassThru=$true;
        RedirectStandardOutput=(Join-Path $SessionDirectory 'radio-host.jsonl');
        RedirectStandardError=(Join-Path $SessionDirectory 'owly-error.log')}
    $taskProcess=Start-Process @taskStart
    $null=$taskProcess.Handle
    $taskStartDeadline=[DateTime]::UtcNow.AddSeconds(45)
    while(-not(Test-Path -LiteralPath $taskRawReady)) {
        if($taskProcess.HasExited) { throw 'The radio host stopped before readiness.' }
        if([DateTime]::UtcNow -ge $taskStartDeadline) { throw 'Radio readiness timed out.' }
        Start-Sleep -Milliseconds 200
    }
    $taskReady=Get-Content -LiteralPath $taskRawReady -Raw | ConvertFrom-Json
    $taskIp=[Net.IPAddress]::Parse($taskReady.address)
    if($taskReady.version -ne 1 -or $taskReady.pid -ne $taskProcess.Id -or
        $taskReady.ibss_configured -ne $true -or -not $taskIp.IsIPv6LinkLocal -or
        $taskIp.ScopeId -ne 0 -or $taskReady.interface_index -le 0) {
        throw 'Invalid native radio readiness record.'
    }
    $taskInterface=Get-NetIPInterface -InterfaceIndex $taskReady.interface_index -AddressFamily IPv6
    $taskAlias="OpenDrop AWDL $($taskProcess.Id)"
    if(@($taskInterface).Count -ne 1 -or $taskInterface.InterfaceAlias -ne $taskAlias) {
        throw 'The radio interface does not belong to this native host.'
    }
    $taskAddressDeadline=[DateTime]::UtcNow.AddSeconds(5)
    do {
        $taskAddresses=@(Get-NetIPAddress -InterfaceIndex $taskReady.interface_index -AddressFamily IPv6)
        $taskAddresses | Select-Object IPAddress,InterfaceIndex,AddressState,PrefixOrigin,SuffixOrigin |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $SessionDirectory 'interface-addresses.json') -Encoding UTF8
        $taskAddressMatches=@($taskAddresses | Where-Object {
            $taskAssigned=[Net.IPAddress]::Parse($_.IPAddress)
            if($taskAssigned.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetworkV6) {
                # Get-NetIPAddress can include the interface scope on link-local
                # addresses. The interface was already checked independently.
                $taskAssigned.ScopeId=0
                $taskAssigned.Equals($taskIp)
            }
        }).Count -gt 0
        if($taskAddressMatches) { break }
        if($taskProcess.HasExited) { throw 'The radio stopped during address verification.' }
        Start-Sleep -Milliseconds 200
    } while([DateTime]::UtcNow -lt $taskAddressDeadline)
    if(-not $taskAddressMatches) {
        throw 'The radio IPv6 address is not assigned to this interface; see interface-addresses.json.'
    }
    $taskCommon=@{Action='Allow';Enabled='True';Profile='Any';InterfaceAlias=$taskAlias;ErrorAction='Stop'}
    $taskSpecs=@(
        @{Direction='Inbound';Protocol='TCP';LocalPort=8771;Program=$PythonProgram;RemoteAddress='fe80::/10'},
        @{Direction='Inbound';Protocol='UDP';LocalPort=5353;Program=$PythonProgram;RemoteAddress='fe80::/10'},
        @{Direction='Outbound';Protocol='UDP';RemotePort=5353;Program=$PythonProgram;RemoteAddress=@('fe80::/10','ff02::fb')},
        @{Direction='Inbound';Protocol='ICMPv6';IcmpType=@(135,136);RemoteAddress=@('fe80::/10','ff02::/16')},
        @{Direction='Outbound';Protocol='ICMPv6';IcmpType=@(135,136);RemoteAddress=@('fe80::/10','ff02::/16')}
    )
    for($taskI=0;$taskI -lt $taskSuffixes.Count;$taskI++) {
        $taskRule="$RulePrefix-$($taskSuffixes[$taskI])"
        $taskSpec=$taskSpecs[$taskI]
        $null=New-NetFirewallRule @taskCommon @taskSpec -Name $taskRule -DisplayName $taskRule
    }
    # Close mDNS/TLS before the bounded host removes its network adapter.
    if(-not $AppSession) {
    $taskStopTime=([DateTimeOffset](Get-Item -LiteralPath $taskRawReady).LastWriteTimeUtc).AddSeconds($Seconds-5)
    $taskReady | Add-Member -NotePropertyName receiver_stop_unix_ms -NotePropertyValue $taskStopTime.ToUnixTimeMilliseconds()
    }
    $taskReady | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $SessionDirectory 'receiver-ready.tmp') -Encoding UTF8
    Move-Item -LiteralPath (Join-Path $SessionDirectory 'receiver-ready.tmp') -Destination (Join-Path $SessionDirectory 'receiver-ready.json')
    if($AppSession) {
        while(-not $taskProcess.WaitForExit(1000)) {
            $taskFresh=(Test-Path -LiteralPath $taskLease) -and
                ([DateTime]::UtcNow-(Get-Item -LiteralPath $taskLease).LastWriteTimeUtc).TotalSeconds -lt 35
            if(-not $taskFresh -and -not $taskProcess.WaitForExit(10000)) {
                throw 'The radio host did not stop after its app lease expired.'
            }
        }
    } elseif(-not $taskProcess.WaitForExit(($Seconds+15)*1000)) {
        throw 'The bounded radio host did not stop on time.'
    }
    $taskProcess.WaitForExit()
    $taskEvents=@(Get-Content -LiteralPath (Join-Path $SessionDirectory 'radio-host.jsonl') |
        ForEach-Object { $_ | ConvertFrom-Json })
    $taskStopped=$taskEvents | Where-Object event -eq 'stopped' | Select-Object -Last 1
    $taskResult.buffers_released=$taskStopped.buffers_released -eq $true
    $taskResult.ok=$taskProcess.ExitCode -eq 0 -and $taskResult.buffers_released
    $taskResult.radio_stop=$taskStopped
    if(-not $taskResult.ok) { throw 'The native radio host did not report verified shutdown.' }
    return 0
} finally {
    if($null -ne $taskProcess) {
        if(-not $taskProcess.HasExited) {
            $taskProcess.Kill()
            $null=$taskProcess.WaitForExit(10000)
        }
        $taskProcess.Dispose()
    }
    $taskResult | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $SessionDirectory 'firmware-init.json') -Encoding UTF8
    $taskResult | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $SessionDirectory 'receiver-ended.json') -Encoding UTF8
    foreach($taskSuffix in $taskSuffixes) {
        Get-NetFirewallRule -Name "$RulePrefix-$taskSuffix" -ErrorAction SilentlyContinue |
            Remove-NetFirewallRule -ErrorAction Stop
    }
}
