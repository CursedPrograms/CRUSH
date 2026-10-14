; CRUSH installer (Inno Setup 6). Built by build_release.bat.
;   - installs crush.exe, adds it to PATH
;   - .crush files get the CRUSH icon; double-click lists the contents
;   - right-click: "Add to CRUSH archive" on files and folders,
;                  "Extract to <name>\" and "Test archive" on .crush files
; Installs for everyone (admin) or just you, your choice; uninstalls cleanly, PATH included.

#define AppName "CRUSH"
#define AppVersion "3.0.0"
#define AppPublisher "Cursed Entertainment"
#define AppURL "https://cursedprograms.github.io/CRUSH/"

[Setup]
AppId={{6E3B1F7A-2C1D-4A5B-9C0E-7F3A1B2C3D4E}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL=https://github.com/CursedPrograms/CRUSH/issues
AppUpdatesURL=https://github.com/CursedPrograms/CRUSH/releases
DefaultDirName={autopf}\CRUSH
DefaultGroupName=CRUSH
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
ChangesAssociations=yes
ChangesEnvironment=yes
LicenseFile=..\LICENSE
SetupIconFile=crush.ico
UninstallDisplayIcon={app}\crush.exe
WizardStyle=modern
OutputDir=..\dist
OutputBaseFilename=CRUSH-{#AppVersion}-setup
Compression=lzma2/ultra64
SolidCompression=yes

[Tasks]
Name: "path"; Description: "Add crush to PATH (use it from any terminal)"
Name: "assoc"; Description: "Open .crush files with CRUSH (icon, double-click lists them)"
Name: "menu"; Description: "Explorer right-click menu: Add to CRUSH archive / Extract / Test"

[Files]
Source: "..\dist\crush.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "crush.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\CRUSH command prompt"; Filename: "{cmd}"; Parameters: "/k ""{app}\crush.exe"""; WorkingDir: "{userdocs}"; IconFilename: "{app}\crush.ico"
Name: "{group}\Read me"; Filename: "{app}\README.txt"
Name: "{group}\Website"; Filename: "{#AppURL}"
Name: "{group}\Uninstall CRUSH"; Filename: "{uninstallexe}"

[Registry]
; .crush -> CRUSH.Archive
Root: HKA; Subkey: "Software\Classes\.crush"; ValueType: string; ValueName: ""; ValueData: "CRUSH.Archive"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive"; ValueType: string; ValueName: ""; ValueData: "CRUSH archive"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\crush.ico"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\open"; ValueType: string; ValueName: ""; ValueData: "List contents"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\open\command"; ValueType: string; ValueName: ""; ValueData: "cmd.exe /c """"{app}\crush.exe"" l ""%1"" & pause"""; Tasks: assoc
; right-click on a .crush file
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\crushextract"; ValueType: string; ValueName: ""; ValueData: "Extract to folder"; Tasks: assoc and menu
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\crushextract"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\crush.ico"; Tasks: assoc and menu
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\crushextract\command"; ValueType: string; ValueName: ""; ValueData: "cmd.exe /c """"{app}\crush.exe"" e ""%1"" & pause"""; Tasks: assoc and menu
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\crushtest"; ValueType: string; ValueName: ""; ValueData: "Test archive"; Tasks: assoc and menu
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\crushtest"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\crush.ico"; Tasks: assoc and menu
Root: HKA; Subkey: "Software\Classes\CRUSH.Archive\shell\crushtest\command"; ValueType: string; ValueName: ""; ValueData: "cmd.exe /c """"{app}\crush.exe"" t ""%1"" & pause"""; Tasks: assoc and menu
; right-click on any file or folder
Root: HKA; Subkey: "Software\Classes\*\shell\crushadd"; ValueType: string; ValueName: ""; ValueData: "Add to CRUSH archive"; Flags: uninsdeletekey; Tasks: menu
Root: HKA; Subkey: "Software\Classes\*\shell\crushadd"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\crush.ico"; Tasks: menu
Root: HKA; Subkey: "Software\Classes\*\shell\crushadd\command"; ValueType: string; ValueName: ""; ValueData: "cmd.exe /c """"{app}\crush.exe"" a ""%1.crush"" ""%1"" & pause"""; Tasks: menu
Root: HKA; Subkey: "Software\Classes\Directory\shell\crushadd"; ValueType: string; ValueName: ""; ValueData: "Add to CRUSH archive"; Flags: uninsdeletekey; Tasks: menu
Root: HKA; Subkey: "Software\Classes\Directory\shell\crushadd"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\crush.ico"; Tasks: menu
Root: HKA; Subkey: "Software\Classes\Directory\shell\crushadd\command"; ValueType: string; ValueName: ""; ValueData: "cmd.exe /c """"{app}\crush.exe"" a ""%1.crush"" ""%1"" & pause"""; Tasks: menu

[Code]
{ PATH: added to the machine PATH for an admin install, the user PATH otherwise, and removed again on uninstall }
function EnvKey(): string;
begin
  if IsAdminInstallMode then
    Result := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  else
    Result := 'Environment';
end;

function EnvRoot(): Integer;
begin
  if IsAdminInstallMode then Result := HKEY_LOCAL_MACHINE else Result := HKEY_CURRENT_USER;
end;

procedure AddToPath(Dir: string);
var Paths: string;
begin
  if not RegQueryStringValue(EnvRoot(), EnvKey(), 'Path', Paths) then Paths := '';
  if Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';') > 0 then exit;
  if (Paths <> '') and (Copy(Paths, Length(Paths), 1) <> ';') then Paths := Paths + ';';
  RegWriteExpandStringValue(EnvRoot(), EnvKey(), 'Path', Paths + Dir);
end;

procedure RemoveFromPath(Dir: string);
var Paths: string; P: Integer;
begin
  if not RegQueryStringValue(EnvRoot(), EnvKey(), 'Path', Paths) then exit;
  Paths := ';' + Paths + ';';
  P := Pos(';' + Uppercase(Dir) + ';', Uppercase(Paths));
  if P = 0 then exit;
  Delete(Paths, P, Length(Dir) + 1);
  Paths := Copy(Paths, 2, Length(Paths) - 2);
  RegWriteExpandStringValue(EnvRoot(), EnvKey(), 'Path', Paths);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('path') then
    AddToPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
