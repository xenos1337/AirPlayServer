; Inno Setup script for the AirPlay Server installer.
; CI compiles it with:
;   ISCC /DAppVersion=1.2.3 /DSourceDir=<staged release files> /O<output dir> installer\AirPlayServer.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\x64\Release"
#endif

#define AppName "AirPlay Server"
#define AppExe "AirPlayServer.exe"
#define RepoUrl "https://github.com/xenos1337/AirPlayServer"
#define DotNetInstaller "windowsdesktop-runtime-win-x64.exe"

[Setup]
AppId={{0EEFE65C-D34E-468F-B0B0-3D05190DB85B}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=xenos1337
AppPublisherURL={#RepoUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputBaseFilename=AirPlayServer-Setup-x64
SetupIconFile=..\AirPlayServer\AirPlayServer.ico
UninstallDisplayIcon={app}\{#AppExe}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
PrivilegesRequired=admin
; The app holds this mutex (see App.xaml.cs); Setup asks to close it first.
AppMutex=Local\AirPlayServer.Instance
CloseApplications=yes
; Only the app's own sign-in entry under HKCU is touched, on uninstall.
UsedUserAreasWarning=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Remove the "Start when you sign in" entry the app may have created.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "AirPlayServer"; ValueType: none; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{tmp}\{#DotNetInstaller}"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the .NET 10 Desktop Runtime..."; Check: DotNetDownloaded; Flags: waituntilterminated
; Devices must reach the receiver; allow it on private networks.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=all dir=in program=""{app}\{#AppExe}"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#AppName}"" dir=in action=allow program=""{app}\{#AppExe}"" enable=yes profile=private,domain"; StatusMsg: "Allowing AirPlay Server through Windows Firewall..."; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=all dir=in program=""{app}\{#AppExe}"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFirewallRule"

[Code]
var
  DownloadPage: TDownloadWizardPage;
  DotNetFetched: Boolean;

{ .NET records installed shared runtimes in the 32-bit registry view. }
function IsDotNet10DesktopInstalled: Boolean;
var
  Versions: TArrayOfString;
  I: Integer;
begin
  Result := False;
  if RegGetValueNames(HKLM32, 'SOFTWARE\dotnet\Setup\InstalledVersions\x64\sharedfx\Microsoft.WindowsDesktop.App', Versions) then
    for I := 0 to GetArrayLength(Versions) - 1 do
      if Copy(Versions[I], 1, 3) = '10.' then
        Result := True;
end;

function DotNetDownloaded: Boolean;
begin
  Result := DotNetFetched;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), nil);
end;

{ Fetch the runtime from Microsoft before installing if it is missing. }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID <> wpReady) or IsDotNet10DesktopInstalled then
    Exit;

  DownloadPage.Clear;
  DownloadPage.Add('https://aka.ms/dotnet/10.0/{#DotNetInstaller}', '{#DotNetInstaller}', '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      DotNetFetched := True;
    except
      if SuppressibleMsgBox('The .NET 10 Desktop Runtime could not be downloaded: ' + GetExceptionMessage + #13#10#13#10 +
        'AirPlay Server needs it to start. Install it later from https://dotnet.microsoft.com/download/dotnet/10.0.' + #13#10#13#10 +
        'Continue without it?', mbError, MB_YESNO, IDNO) = IDNO then
        Result := False;
    end;
  finally
    DownloadPage.Hide;
  end;
end;
