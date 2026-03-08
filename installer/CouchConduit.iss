;  ============================================================
;  Couch Conduit — Inno Setup Installer Script
;  ============================================================
;  Build with: iscc.exe installer\CouchConduit.iss
;  Or open in Inno Setup Compiler GUI.
;
;  Prerequisites:
;    - Inno Setup 6.x  (https://jrsoftware.org/isinfo.php)
;    - Build the project first: cmake --build build --config Release
;  ============================================================

#define MyAppName      "Couch Conduit"
#define MyAppVersion   "0.1.0"
#define MyAppPublisher "Couch Conduit Contributors"
#define MyAppURL       "https://github.com/acLebert/Couch_Conduit"
#define MyAppHostExe   "cc_host.exe"
#define MyAppClientExe "cc_client.exe"

; Paths relative to the .iss file location (installer/)
#define ProjectRoot    ".."
#define BuildHost      ProjectRoot + "\build\src\host\Release"
#define BuildClient    ProjectRoot + "\build\src\client\Release"
#define FFmpegBin      ProjectRoot + "\third_party\ffmpeg\bin"

[Setup]
AppId={{7E3A0C21-B5F4-4D9A-A8E7-CC2D1F8B3E6A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile={#ProjectRoot}\LICENSE
OutputDir={#ProjectRoot}\dist
OutputBaseFilename=CouchConduit-{#MyAppVersion}-x64-setup
SetupIconFile=
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppHostExe}
MinVersion=10.0.17763

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";    Description: "Full installation (Host + Client)"
Name: "host";    Description: "Host only (stream your PC)"
Name: "client";  Description: "Client only (view remote stream)"
Name: "custom";  Description: "Custom installation"; Flags: iscustom

[Components]
Name: "host";      Description: "Host (stream sender)";   Types: full host
Name: "client";    Description: "Client (stream viewer)";  Types: full client
Name: "ffmpeg";    Description: "FFmpeg runtime DLLs";     Types: full host client; Flags: fixed
Name: "docs";      Description: "Documentation";           Types: full

[Files]
; Host executable
Source: "{#BuildHost}\{#MyAppHostExe}";   DestDir: "{app}"; Components: host;   Flags: ignoreversion

; Client executable
Source: "{#BuildClient}\{#MyAppClientExe}"; DestDir: "{app}"; Components: client; Flags: ignoreversion

; FFmpeg runtime DLLs
Source: "{#FFmpegBin}\avcodec-62.dll";     DestDir: "{app}"; Components: ffmpeg; Flags: ignoreversion
Source: "{#FFmpegBin}\avutil-60.dll";      DestDir: "{app}"; Components: ffmpeg; Flags: ignoreversion
Source: "{#FFmpegBin}\swresample-5.dll";   DestDir: "{app}"; Components: ffmpeg; Flags: ignoreversion
; Include any other FFmpeg DLLs present
Source: "{#FFmpegBin}\*.dll";              DestDir: "{app}"; Components: ffmpeg; Flags: ignoreversion skipifsourcedoesntexist

; Documentation
Source: "{#ProjectRoot}\README.md";    DestDir: "{app}"; Components: docs; Flags: ignoreversion
Source: "{#ProjectRoot}\LICENSE";      DestDir: "{app}"; Components: docs; Flags: ignoreversion
Source: "{#ProjectRoot}\TESTING.md";   DestDir: "{app}"; Components: docs; Flags: ignoreversion skipifsourcedoesntexist

; Quick-start scripts
Source: "{#ProjectRoot}\scripts\start-host.bat";   DestDir: "{app}"; Components: host;   Flags: ignoreversion skipifsourcedoesntexist
Source: "{#ProjectRoot}\scripts\start-client.bat"; DestDir: "{app}"; Components: client; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\Couch Conduit Host";          Filename: "{app}\{#MyAppHostExe}";   Components: host
Name: "{group}\Couch Conduit Client";        Filename: "{app}\{#MyAppClientExe}"; Components: client
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\Couch Conduit Host";  Filename: "{app}\{#MyAppHostExe}";   Components: host;   Tasks: desktopicon
Name: "{commondesktop}\Couch Conduit Client";Filename: "{app}\{#MyAppClientExe}"; Components: client; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Registry]
; Add install directory to PATH (user-level) so cc_host / cc_client can be invoked from terminal
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
    ValueData: "{olddata};{app}"; Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
; Post-install: optionally launch the selected component
Filename: "{app}\{#MyAppHostExe}";   Description: "Launch Couch Conduit Host";   Components: host;   Flags: postinstall nowait skipifsilent unchecked
Filename: "{app}\{#MyAppClientExe}"; Description: "Launch Couch Conduit Client"; Components: client; Flags: postinstall nowait skipifsilent unchecked

[Code]
// Helper: check if {app} is already on PATH
function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

// Pre-install check: warn if ViGEmBus is not detected (needed for gamepad support)
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;

  // Check for ViGEmBus driver (optional)
  if not RegKeyExists(HKEY_LOCAL_MACHINE, 'SYSTEM\CurrentControlSet\Services\ViGEmBus') then
  begin
    if MsgBox(
      'ViGEmBus driver is not installed.' + #13#10 +
      'Gamepad/controller passthrough will not work without it.' + #13#10#13#10 +
      'You can install it later from:' + #13#10 +
      'https://github.com/nefarius/ViGEmBus/releases' + #13#10#13#10 +
      'Continue installation anyway?',
      mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
    end;
  end;
end;

[UninstallDelete]
Type: dirifempty; Name: "{app}"
