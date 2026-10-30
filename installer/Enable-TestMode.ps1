# SPDX-License-Identifier: GPL-3.0-or-later
# Run only after the installer user explicitly selects Test Mode preparation.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$HelperPath,[Parameter(Mandatory)][string]$ResultPath,[Parameter(Mandatory)][string]$BackupDirectory)
$ErrorActionPreference='Stop'
try {
    & $HelperPath -Mode Enable -BackupDirectory $BackupDirectory | Out-Null
    @('[result]','success=1','message=Test Mode is prepared for the next boot. Save your work and restart manually before using AirDrop.') | Set-Content -LiteralPath $ResultPath -Encoding Unicode
} catch {
    @('[result]','success=0',('message='+($_.Exception.Message -replace '[\r\n]',' '))) | Set-Content -LiteralPath $ResultPath -Encoding Unicode
    exit 1
}
