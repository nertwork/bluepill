#ifndef AppVersion
  #define AppVersion "1.2.16"
#endif

#ifndef DistDir
  #define DistDir "..\build-win-dist"
#endif

[Setup]
AppId={{8D6B4F2C-1A7D-4C9B-B7E2-9F53A2B1D4E0}
AppName=BluePill
AppVersion={#AppVersion}
AppVerName=BluePill {#AppVersion}
AppPublisher=Nertwork
AppPublisherURL=https://github.com/nertwork/bluepill
AppSupportURL=https://github.com/nertwork/bluepill/issues
AppUpdatesURL=https://github.com/nertwork/bluepill/releases
DefaultDirName={autopf}\BluePill
DefaultGroupName=BluePill
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=BluePill-Setup-{#AppVersion}-win64
SetupIconFile=..\resources\bluepill.ico
UninstallDisplayIcon={app}\bluepill.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=force
RestartApplications=yes
AppMutex=BluePillSingleInstanceMutex
ChangesEnvironment=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\resources\imessage-notify.wav"; DestDir: "{app}\resources"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\BluePill"; Filename: "{app}\bluepill.exe"
Name: "{group}\BluePill (Demo Mode)"; Filename: "{app}\bluepill.exe"; Parameters: "--demo"
Name: "{group}\{cm:UninstallProgram,BluePill}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\BluePill"; Filename: "{app}\bluepill.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bluepill.exe"; Description: "{cm:LaunchProgram,BluePill}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\bluepill.exe"; Parameters: ""; Flags: nowait runasoriginaluser; Check: WizardSilent

[Code]
function InitializeSetup(): Boolean;
var
  WaitMs: Integer;
begin
  Result := True;
  if WizardSilent then
  begin
    WaitMs := 0;
    while CheckForMutexes('BluePillSingleInstanceMutex') and (WaitMs < 5000) do
    begin
      Sleep(100);
      WaitMs := WaitMs + 100;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and (not UninstallSilent) then
  begin
    if MsgBox('Remove BluePill saved settings, sign-in credentials, and local message cache?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(ExpandConstant('{userappdata}\bluepill'), True, True, True);
      DelTree(ExpandConstant('{localappdata}\bluepill'), True, True, True);
    end;
  end;
end;
