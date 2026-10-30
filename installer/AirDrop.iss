#ifndef PackageDir
  #error PackageDir must point to the locally built app directory
#endif
#ifndef OutputPath
  #error OutputPath is required
#endif
#define Version "0.2.0-preview.1"
[Setup]
AppId={{A3AF0C92-A3AD-4D6D-8CE7-EC64596867D4}
AppName=AirDrop for Windows
AppVersion={#Version}
AppPublisher=21Suspect
AppPublisherURL=https://github.com/21Suspect/AirDrop-for-Windows
DefaultDirName={localappdata}\Programs\AirDrop for Windows
DefaultGroupName=AirDrop for Windows
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.26100
WizardStyle=modern
DisableDirPage=yes
DisableWelcomePage=no
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\AirDrop for Windows.exe
OutputDir={#OutputPath}
OutputBaseFilename=AirDrop-for-Windows-Setup-{#Version}-x64
SetupIconFile=..\app\OpenDrop.ico
Compression=lzma2
SolidCompression=yes
LicenseFile=..\LICENSE
CloseApplications=no
RestartApplications=no
SetupLogging=yes
[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Check-Prerequisites.ps1"; Flags: dontcopy
Source: "Enable-TestMode.ps1"; Flags: dontcopy
Source: "..\native\intel\Set-DriverTestMode.ps1"; Flags: dontcopy
[Icons]
Name: "{userprograms}\AirDrop for Windows"; Filename: "{app}\AirDrop for Windows.exe"
Name: "{userdesktop}\AirDrop for Windows"; Filename: "{app}\AirDrop for Windows.exe"; Tasks: desktopicon
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
[Run]
Filename: "{app}\AirDrop for Windows.exe"; Description: "Open AirDrop for Windows"; Flags: nowait postinstall skipifsilent runasoriginaluser
[Code]
var
  ChecksPage: TWizardPage;
  Details: TNewStaticText;
  PrepareBoot: TNewCheckBox;
  ReportPath: String;
  Supported: Boolean;
  BootPrepared: Boolean;

function PowerShellPath: String;
begin
  if IsWin64 and not IsCurrentProcess64Bit then
    Result := ExpandConstant('{sysnative}\WindowsPowerShell\v1.0\powershell.exe')
  else
    Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

procedure DetectRequirements;
var
  ExitCode: Integer;
  Text: String;
begin
  ExtractTemporaryFile('Check-Prerequisites.ps1');
  ReportPath := ExpandConstant('{tmp}\airdrop-checks.ini');
  Supported := False;
  if not Exec(PowerShellPath, '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{tmp}\Check-Prerequisites.ps1') + '" -OutputPath "' + ReportPath + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
    RaiseException('Could not inspect this PC.');
  if ExitCode <> 0 then RaiseException('PC detection failed. No system settings have been changed.');
  Supported := GetIniString('checks', 'supported', '0', ReportPath) = '1';
  Text := GetIniString('checks', 'hardware', 'Unknown hardware', ReportPath) + #13#10#13#10 +
    'Driver Test Mode: ' + GetIniString('checks', 'testmode', 'Unknown', ReportPath) + #13#10 +
    'Secure Boot: ' + GetIniString('checks', 'secureboot', 'Unknown', ReportPath) + #13#10 +
    'Ethernet: ' + GetIniString('checks', 'ethernet', 'Unknown', ReportPath) + #13#10 +
    'Bluetooth: ' + GetIniString('checks', 'bluetooth', 'Unknown', ReportPath) + #13#10#13#10 +
    GetIniString('checks', 'message', '', ReportPath);
  Details.Caption := Text;
  PrepareBoot.Enabled := Supported and (GetIniString('checks', 'testmode', '', ReportPath) <> 'Enabled');
end;

procedure InitializeWizard;
begin
  ChecksPage := CreateCustomPage(wpWelcome, 'Check this PC', 'Experimental preview - review the requirements before installing');
  Details := TNewStaticText.Create(ChecksPage);
  Details.Parent := ChecksPage.Surface;
  Details.SetBounds(0, 0, ChecksPage.SurfaceWidth, ScaleY(155));
  Details.AutoSize := False;
  Details.WordWrap := True;
  PrepareBoot := TNewCheckBox.Create(ChecksPage);
  PrepareBoot.Parent := ChecksPage.Surface;
  PrepareBoot.SetBounds(0, ScaleY(160), ChecksPage.SurfaceWidth, ScaleY(42));
  PrepareBoot.Caption := 'Prepare Windows Test Mode (administrator approval and a manual restart required)';
  PrepareBoot.Checked := False;
  with TNewStaticText.Create(ChecksPage) do begin
    Parent := ChecksPage.Surface;
    SetBounds(0, ScaleY(210), ChecksPage.SurfaceWidth, ScaleY(64));
    AutoSize := False; WordWrap := True;
    Caption := 'The radio helper temporarily installs the included test driver and trusts its test certificate. Wi-Fi is reserved for AirDrop. Files from anyone nearby are accepted by default; change this in Settings.';
  end;
  DetectRequirements;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ExitCode: Integer;
  ResultPath: String;
begin
  Result := True;
  if CurPageID <> ChecksPage.ID then exit;
  if not Supported then begin
    MsgBox(GetIniString('checks', 'message', 'Unsupported hardware.', ReportPath), mbError, MB_OK);
    Result := False; exit;
  end;
  if PrepareBoot.Checked and not BootPrepared then begin
    if WizardSilent then begin Result := False; exit; end;
    if MsgBox('Test Mode weakens normal driver trust and adds a desktop watermark. This cannot run with Secure Boot enabled. The helper refuses encrypted system volumes, saves a boot configuration backup and never restarts the PC.' + #13#10#13#10 +
      'Prepare Test Mode now?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then begin
      Result := False; exit;
    end;
    ExtractTemporaryFile('Enable-TestMode.ps1');
    ExtractTemporaryFile('Set-DriverTestMode.ps1');
    ResultPath := ExpandConstant('{tmp}\airdrop-boot-result.ini');
    if not ShellExec('runas', PowerShellPath,
      '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\Enable-TestMode.ps1') +
      '" -HelperPath "' + ExpandConstant('{tmp}\Set-DriverTestMode.ps1') +
      '" -ResultPath "' + ResultPath + '" -BackupDirectory "' + ExpandConstant('{localappdata}\AirDropBootBackup') + '"',
      '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then begin
      MsgBox('Administrator approval was not completed. Installation can continue with Test Mode preparation unchecked.', mbError, MB_OK);
      Result := False; exit;
    end;
    BootPrepared := GetIniString('result', 'success', '0', ResultPath) = '1';
    MsgBox(GetIniString('result', 'message', 'Test Mode preparation did not complete. No restart was requested.', ResultPath), mbInformation, MB_OK);
    if not BootPrepared then begin Result := False; exit; end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not Supported then Result := 'This PC does not meet the tested hardware requirements.';
  if CheckForMutexes('Local\OpenDropDesktop') then
    Result := 'Close AirDrop for Windows and wait for Wi-Fi restoration before installing or updating.';
end;

function InitializeUninstall: Boolean;
var
  ExitCode: Integer;
  RemovalReport: String;
begin
  Result := False;
  if CheckForMutexes('Local\OpenDropDesktop') then begin
    MsgBox('Close AirDrop for Windows and let it restore Wi-Fi before uninstalling.', mbError, MB_OK);
    exit;
  end;
  RemovalReport := ExpandConstant('{tmp}\airdrop-removal.ini');
  if not Exec(PowerShellPath, '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{app}\assets\setup\Check-Prerequisites.ps1') + '" -CheckRemoval -OutputPath "' + RemovalReport + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then exit;
  Result := (ExitCode = 0) and (GetIniString('checks', 'removable', '0', RemovalReport) = '1');
  if not Result then MsgBox(GetIniString('checks', 'message', 'Recovery status could not be checked. Keep the app installed until Wi-Fi is restored.', RemovalReport), mbError, MB_OK);
end;
