# SPDX-License-Identifier: GPL-3.0-or-later
# Changes only the current Windows loader's TESTSIGNING option. Never reboots.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Enable','Disable')][string]$Mode,
    [Parameter(Mandatory)][string]$BackupDirectory
)
$ErrorActionPreference='Stop'
$taskPrincipal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run as Administrator.'
}
if($Mode -eq 'Enable') {
    if(Confirm-SecureBootUEFI) { throw 'Secure Boot is enabled. This script does not change firmware security settings.' }
    $taskVolume=Get-BitLockerVolume -MountPoint $env:SystemDrive
    if([string]$taskVolume.ProtectionStatus -ne 'Off' -or [string]$taskVolume.VolumeStatus -ne 'FullyDecrypted') {
        throw 'Encrypted system volume detected. This script will not alter its boot configuration.'
    }
}
$null=New-Item -ItemType Directory -Path $BackupDirectory -Force
$BackupDirectory=(Resolve-Path -LiteralPath $BackupDirectory).Path
$taskBackupFile=Join-Path $BackupDirectory ("bcd-before-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak')
& bcdedit.exe /export $taskBackupFile
if($LASTEXITCODE) { throw 'Boot configuration backup failed; no change made.' }
$taskBefore=@(& bcdedit.exe /enum '{current}')
if($LASTEXITCODE) { throw 'Cannot read the current loader; no change made.' }
$taskValue=if($Mode -eq 'Enable') {'on'} else {'off'}
& bcdedit.exe /set '{current}' testsigning $taskValue
if($LASTEXITCODE) { throw 'Windows rejected the test-signing configuration change.' }
$taskAfter=@(& bcdedit.exe /enum '{current}')
[pscustomobject]@{Mode=$Mode;Before=$taskBefore;After=$taskAfter;Backup=$taskBackupFile;
    ChangedUtc=[DateTime]::UtcNow.ToString('o');RestartRequired=$true} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $BackupDirectory 'last-change.json') -Encoding UTF8
Write-Output 'Boot option updated. It takes effect only after you restart Windows. No restart was initiated.'
