# SPDX-License-Identifier: GPL-3.0-or-later
# Run one owned native helper without PowerShell's NativeCommandError conversion.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FilePath,
    [string[]]$ArgumentList,
    [Parameter(Mandatory)][string]$OutputPath,
    [Parameter(Mandatory)][string]$ErrorPath,
    [ValidateRange(1,120)][int]$TimeoutSeconds=90
)
$ErrorActionPreference='Stop'
$FilePath=(Resolve-Path -LiteralPath $FilePath).Path
$taskProcess=$null
try {
    $taskStart=@{FilePath=$FilePath; ArgumentList=$ArgumentList; WindowStyle='Hidden';
        PassThru=$true; RedirectStandardOutput=$OutputPath; RedirectStandardError=$ErrorPath}
    $taskProcess=Start-Process @taskStart
    # Retain the process handle before waiting, including for very short helpers.
    $null=$taskProcess.Handle
    if(-not $taskProcess.WaitForExit($TimeoutSeconds*1000)) {
        $taskProcess.Kill()
        if(-not $taskProcess.WaitForExit(10000)) {
            throw 'The test helper did not exit after termination; recovery must probe DMA state.'
        }
        throw "The test helper exceeded its $TimeoutSeconds second bound and was stopped."
    }
    $taskProcess.WaitForExit()
    return [int]$taskProcess.ExitCode
} finally {
    if($null -ne $taskProcess) {
        # Dispose releases our process handle; it never stops an unrelated PID.
        $taskProcess.Dispose()
    }
}
